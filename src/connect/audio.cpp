#include "audio.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <curl/curl.h>
#include <mbedtls/aes.h>
#include <whb/log.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>

namespace Connect {

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr size_t CHUNK_SIZE   = 0x20000;   // 128 KB per fetch
static constexpr size_t PCM_RING_CAP = 1u << 16;  // 65536 samples — power-of-2 required
static constexpr int    SAMPLE_RATE  = 44100;
static constexpr int    CHANNELS     = 2;

// Spotify audio AES-128-CTR IV (from librespot decrypt.rs AUDIO_AESIV constant).
// The counter starts here and increments by 1 for every 16-byte AES block,
// matching Ctr128BE — big-endian 128-bit counter.
static const uint8_t AUDIO_AESIV[16] = {
    0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
    0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93,
};

// ── AES-CTR decrypt ───────────────────────────────────────────────────────────

static void aes_ctr_decrypt(const std::vector<uint8_t> &key,
                              size_t file_offset,
                              uint8_t *data, size_t len) {
    if (!len) return;

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key.data(), 128);

    // nonce = AUDIO_AESIV + block_count  (128-bit big-endian addition)
    uint8_t nonce[16];
    memcpy(nonce, AUDIO_AESIV, 16);
    uint64_t block_count = (uint64_t)(file_offset / 16);
    uint32_t carry = 0;
    for (int i = 15; i >= 0 && (block_count || carry); i--) {
        uint32_t val = nonce[i] + (block_count & 0xFF) + carry;
        nonce[i]    = (uint8_t)(val & 0xFF);
        carry       = val >> 8;
        block_count >>= 8;
    }

    // nc_off = position within the starting 16-byte AES block (0–15)
    size_t  nc_off       = file_offset % 16;
    uint8_t stream_block[16] = {};

    // If not block-aligned, prime the partial first keystream block
    if (nc_off != 0) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, nonce, stream_block);
        for (int i = 15; i >= 0; --i) if (++nonce[i]) break;
    }

    mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce, stream_block, data, data);
    mbedtls_aes_free(&aes);
}

// ── HTTP range fetch ──────────────────────────────────────────────────────────

static size_t curl_write_bytes(char *ptr, size_t sz, size_t n, void *ud) {
    auto *v = static_cast<std::vector<uint8_t> *>(ud);
    v->insert(v->end(), reinterpret_cast<uint8_t *>(ptr),
                         reinterpret_cast<uint8_t *>(ptr) + sz * n);
    return sz * n;
}

static size_t curl_header_size(char *buf, size_t sz, size_t n, void *ud) {
    auto *total = static_cast<size_t *>(ud);
    std::string line(buf, sz * n);
    // "Content-Range: bytes start-end/total"
    auto pos = line.find('/');
    if ((line.find("Content-Range") != std::string::npos ||
         line.find("content-range") != std::string::npos) && pos != std::string::npos) {
        size_t v = (size_t)strtoul(line.c_str() + pos + 1, nullptr, 10);
        if (v > 0) *total = v;
    }
    return sz * n;
}

static int fetch_abort_check(void *ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return reinterpret_cast<const std::atomic<bool> *>(ud)->load() ? 1 : 0;
}

static bool fetch_range(const std::string &url, size_t start, size_t end_inc,
                         std::vector<uint8_t> &out, size_t &file_size,
                         const std::atomic<bool> *stop = nullptr) {
    char range_str[64];
    snprintf(range_str, sizeof(range_str), "%zu-%zu", start, end_inc);

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_RANGE,           range_str);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   curl_write_bytes);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        &out);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,   curl_header_size);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,       &file_size);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,   0L);
    if (stop) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, fetch_abort_check);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     stop);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    }

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && !out.empty();
}

// ── Chunk cache ───────────────────────────────────────────────────────────────

static const char *CACHE_BASE = "/vol/external01/spotify_cache";

static std::string to_hex(const uint8_t *data, size_t len) {
    static const char h[] = "0123456789abcdef";
    std::string s; s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) { s += h[data[i] >> 4]; s += h[data[i] & 0xF]; }
    return s;
}

struct ChunkCache {
    struct Slot {
        size_t               file_offset = SIZE_MAX;
        std::vector<uint8_t> data;
    };
    static constexpr int SLOTS = 4;
    Slot        slots[SLOTS];
    int         next = 0;
    std::string cache_dir;   // set once per track; empty = SD cache disabled

    void set_file_id(const std::vector<uint8_t> &file_id) {
        if (file_id.empty()) { cache_dir.clear(); return; }
        std::string hex = to_hex(file_id.data(), file_id.size());
        cache_dir = std::string(CACHE_BASE) + "/" + hex;
        mkdir(CACHE_BASE, 0755);
        mkdir(cache_dir.c_str(), 0755);
        // Write/refresh .timestamp: line 1 = first-cached unix ts (immutable),
        // line 2 = last-played unix ts (updated on every play).
        std::string ts_path = cache_dir + "/.timestamp";
        long long cached_at = 0;
        FILE *f = fopen(ts_path.c_str(), "r");
        if (f) { fscanf(f, "%lld", &cached_at); fclose(f); }
        long long now = (long long)time(nullptr);
        if (cached_at == 0) cached_at = now;
        f = fopen(ts_path.c_str(), "w");
        if (f) { fprintf(f, "%lld\n%lld\n", cached_at, now); fclose(f); }
    }

    std::string chunk_path(size_t chunk_idx) const {
        char buf[16]; snprintf(buf, sizeof(buf), "/%04zu", chunk_idx);
        return cache_dir + buf;
    }

    bool load_from_sd(Slot &slot, size_t chunk_idx) {
        if (cache_dir.empty()) return false;
        FILE *f = fopen(chunk_path(chunk_idx).c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f); rewind(f);
        if (sz <= 0) { fclose(f); return false; }
        slot.data.resize((size_t)sz);
        bool ok = fread(slot.data.data(), 1, (size_t)sz, f) == (size_t)sz;
        fclose(f);
        if (!ok) slot.data.clear();
        return ok;
    }

    void save_to_sd(size_t chunk_idx, const uint8_t *data, size_t sz) {
        if (cache_dir.empty()) return;
        FILE *f = fopen(chunk_path(chunk_idx).c_str(), "wb");
        if (!f) return;
        fwrite(data, 1, sz, f);
        fclose(f);
    }

    // Returns pointer to decrypted chunk data (nullptr on fetch failure or abort).
    const uint8_t *get(const std::string &url,
                        const std::vector<uint8_t> &key,
                        size_t chunk_idx, size_t file_size,
                        const std::atomic<bool> *stop = nullptr) {
        size_t off = chunk_idx * CHUNK_SIZE;

        // 1. Memory hit
        for (auto &s : slots)
            if (s.file_offset == off) return s.data.data();

        Slot &slot = slots[next]; next = (next + 1) % SLOTS;
        slot.file_offset = off;
        slot.data.clear();

        // 2. SD cache hit (already decrypted)
        if (load_from_sd(slot, chunk_idx)) {
            slot.file_offset = off;
            return slot.data.data();
        }

        // 3. CDN fetch + decrypt
        size_t end = std::min(off + CHUNK_SIZE - 1, file_size - 1);
        size_t dummy = 0;
        if (!fetch_range(url, off, end, slot.data, dummy, stop)) {
            slot.file_offset = SIZE_MAX;
            return nullptr;
        }
        aes_ctr_decrypt(key, off, slot.data.data(), slot.data.size());

        // 4. Persist decrypted chunk to SD
        save_to_sd(chunk_idx, slot.data.data(), slot.data.size());

        return slot.data.data();
    }
};

// ── Decode context (owned by decode_thread_) ──────────────────────────────────

struct DecodeCtx {
    std::string               cdn_url;
    std::vector<uint8_t>      aes_key;
    size_t                    file_size   = 0;
    size_t                    data_offset = 0;
    size_t                    read_pos    = 0;
    ChunkCache                cache;
    const std::atomic<bool>  *stop_flag  = nullptr;

    // Visible Vorbis stream size (file_size minus header)
    size_t audio_size() const {
        return file_size > data_offset ? file_size - data_offset : 0;
    }
    // Translate Vorbis-stream-relative position to file position
    size_t file_pos() const { return read_pos + data_offset; }
};

// ── Vorbis ov_callbacks ───────────────────────────────────────────────────────

size_t ov_read_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    auto *ctx = static_cast<DecodeCtx *>(ud);
    size_t want = size * nmemb;
    size_t asiz = ctx->audio_size();
    if (want == 0 || asiz == 0 || ctx->read_pos >= asiz) return 0;
    want = std::min(want, asiz - ctx->read_pos);

    size_t done = 0;
    while (done < want) {
        size_t fp  = ctx->data_offset + ctx->read_pos;  // file position tracks read_pos
        size_t ci  = fp / CHUNK_SIZE;
        size_t oi  = fp % CHUNK_SIZE;

        const uint8_t *chunk = ctx->cache.get(ctx->cdn_url, ctx->aes_key,
                                               ci, ctx->file_size, ctx->stop_flag);
        if (!chunk) { WHBLogPrint("audio: chunk fetch failed"); return 0; }

        size_t chunk_end = std::min((ci + 1) * CHUNK_SIZE, ctx->file_size);
        size_t avail     = std::min(chunk_end - fp, want - done);

        memcpy(static_cast<uint8_t *>(ptr) + done, chunk + oi, avail);
        done          += avail;
        ctx->read_pos += avail;
    }
    return done;
}

int ov_seek_cb(void *ud, ogg_int64_t offset, int whence) {
    auto *ctx = static_cast<DecodeCtx *>(ud);
    size_t asiz = ctx->audio_size();
    int64_t pos;
    switch (whence) {
    case SEEK_SET: pos = offset; break;
    case SEEK_CUR: pos = (int64_t)ctx->read_pos + offset; break;
    case SEEK_END: pos = (int64_t)asiz + offset; break;
    default: return -1;
    }
    if (pos < 0 || (size_t)pos > asiz) return -1;
    ctx->read_pos = (size_t)pos;
    return 0;
}

int  ov_close_cb(void * /*ud*/) { return 0; }
long ov_tell_cb(void *ud) {
    return (long)static_cast<DecodeCtx *>(ud)->read_pos;  // Vorbis-relative position
}

// ── SDL audio callback ────────────────────────────────────────────────────────

void AudioPipeline::sdl_audio_cb(void *userdata, uint8_t *stream, int len) {
    static_cast<AudioPipeline *>(userdata)->fill_audio(stream, len);
}

void AudioPipeline::fill_audio(uint8_t *stream, int len) {
    int16_t *out   = reinterpret_cast<int16_t *>(stream);
    size_t   cnt   = (size_t)len / sizeof(int16_t);
    int      vgain = vol_gain_.load();
    int      xgain = crystal_enabled_.load() ? crystal_gain_.load() : 0;

    std::lock_guard<std::mutex> lk(pcm_mu_);
    size_t avail = pcm_write_ - pcm_read_;
    // Align to stereo frame so L/R indices stay consistent.
    size_t fill  = std::min(cnt, avail) & ~size_t(1);

    for (size_t i = 0; i < fill; i += 2) {
        int32_t sl = pcm_buf_[(pcm_read_ + i)     & (PCM_RING_CAP - 1)];
        int32_t sr = pcm_buf_[(pcm_read_ + i + 1) & (PCM_RING_CAP - 1)];

        // Volume (quadratic log-scale)
        sl = (sl * vgain) >> 10;
        sr = (sr * vgain) >> 10;

        // Crystalizer: y[n] = x[n] + gain/256 * (x[n] - x[n-1])
        // Boosts transients and high frequencies. prev tracks the input (pre-effect)
        // so toggling the effect on/off doesn't cause a click.
        int32_t out_l = sl, out_r = sr;
        if (xgain) {
            out_l = sl + ((sl - (int32_t)crystal_prev_l_) * xgain >> 8);
            out_r = sr + ((sr - (int32_t)crystal_prev_r_) * xgain >> 8);
            out_l = std::clamp(out_l, (int32_t)-32768, (int32_t)32767);
            out_r = std::clamp(out_r, (int32_t)-32768, (int32_t)32767);
        }
        crystal_prev_l_ = (int16_t)std::clamp(sl, (int32_t)-32768, (int32_t)32767);
        crystal_prev_r_ = (int16_t)std::clamp(sr, (int32_t)-32768, (int32_t)32767);

        out[i]     = (int16_t)out_l;
        out[i + 1] = (int16_t)out_r;
    }
    pcm_read_ += fill;

    if (fill < cnt) {
        // Reset prev on underrun so the next audio burst starts cleanly.
        crystal_prev_l_ = crystal_prev_r_ = 0;
        memset(out + fill, 0, (cnt - fill) * sizeof(int16_t));
    }
}

// ── Decode thread ─────────────────────────────────────────────────────────────

void AudioPipeline::decode_thread_fn() {
    OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU2);
    DecodeCtx ctx;
    ctx.cdn_url   = cdn_url_;
    ctx.aes_key   = aes_key_;
    ctx.stop_flag = &stop_flag_;
    ctx.cache.set_file_id(file_id_);

    // Fetch chunk 0 to discover file size
    {
        size_t sz = 0;
        std::vector<uint8_t> tmp;
        if (!fetch_range(ctx.cdn_url, 0, CHUNK_SIZE - 1, tmp, sz, &stop_flag_) || sz == 0) {
            WHBLogPrint("audio: initial fetch failed");
            if (on_track_end) on_track_end();
            return;
        }
        ctx.file_size = sz;
        // Seed the cache with chunk 0
        aes_ctr_decrypt(ctx.aes_key, 0, tmp.data(), tmp.size());
        ctx.cache.slots[0] = {0, std::move(tmp)};
        ctx.cache.next = 1;
        WHBLogPrintf("audio: file_size=%zu", ctx.file_size);
    }

    // Detect and skip the Spotify OGG metadata header page.
    // It has header_type=0x06 (BOS+EOS) and is followed by the real Vorbis stream.
    // Scan for the second "OggS" in the decrypted data to find the Vorbis start.
    {
        const auto &d = ctx.cache.slots[0].data;
        // Start scanning from byte 4 to skip the first "OggS"
        for (size_t i = 4; i + 3 < d.size(); i++) {
            if (d[i]   == 0x4F && d[i+1] == 0x67 &&
                d[i+2] == 0x67 && d[i+3] == 0x53) {
                ctx.data_offset = i;
                WHBLogPrintf("audio: Spotify OGG header %zu bytes; Vorbis at %zu",
                             i, i);
                break;
            }
        }
        if (ctx.data_offset == 0)
            WHBLogPrint("audio: no Spotify OGG header found — reading from byte 0");
    }

    // Open Vorbis
    ov_callbacks cbs = {ov_read_cb, ov_seek_cb, ov_close_cb, ov_tell_cb};
    if (ov_open_callbacks(&ctx, &vf_, nullptr, 0, cbs) < 0) {
        WHBLogPrint("audio: ov_open_callbacks failed");
        if (on_track_end) on_track_end();
        return;
    }
    vf_open_ = true;

    // Apply start seek (Tremor ov_time_seek uses milliseconds)
    int start = seek_ms_.exchange(-1);
    if (start > 0) ov_time_seek(&vf_, (ogg_int64_t)start);

    playing_.store(true);
    WHBLogPrint("audio: decode loop");

    while (!stop_flag_.load()) {
        if (paused_.load()) { OSSleepTicks(OSMillisecondsToTicks(10)); continue; }

        // Check for pending seek
        int sm = seek_ms_.exchange(-1);
        if (sm >= 0) ov_time_seek(&vf_, (ogg_int64_t)sm);

        // Wait if ring buffer is nearly full
        {
            std::lock_guard<std::mutex> lk(pcm_mu_);
            size_t used = pcm_write_ - pcm_read_;
            if (used + 2048 >= PCM_RING_CAP) {
                OSSleepTicks(OSMillisecondsToTicks(5));
                continue;
            }
        }

        // Decode a block of PCM (Tremor returns native-endian signed 16-bit)
        char buf[4096];
        int  bs  = 0;
        long ret = ov_read(&vf_, buf, (int)sizeof(buf), &bs);
        if (ret < 0)  { WHBLogPrintf("audio: ov_read error %ld", ret); break; }
        if (ret == 0) { WHBLogPrint("audio: end of stream"); break; }

        size_t samples = (size_t)ret / sizeof(int16_t);
        {
            std::lock_guard<std::mutex> lk(pcm_mu_);
            for (size_t i = 0; i < samples; ++i) {
                pcm_buf_[pcm_write_ & (PCM_RING_CAP - 1)] =
                    reinterpret_cast<int16_t *>(buf)[i];
                ++pcm_write_;
            }
        }

        // Update position (Tremor ov_time_tell returns milliseconds)
        pos_ms_atomic_.store((int)ov_time_tell(&vf_));
    }

    if (vf_open_) { ov_clear(&vf_); vf_open_ = false; }
    playing_.store(false);

    if (!stop_flag_.load() && on_track_end) on_track_end();
    WHBLogPrint("audio: decode thread exited");
}

// ── AX DRC stubs ──────────────────────────────────────────────────────────────

bool AudioPipeline::ax_init()                              { return true; }
void AudioPipeline::ax_shutdown()                          {}
void AudioPipeline::ax_push(const int16_t * /*p*/, size_t /*n*/) {}

// ── Public interface ──────────────────────────────────────────────────────────

AudioPipeline::AudioPipeline()  = default;
AudioPipeline::~AudioPipeline() { shutdown(); }

bool AudioPipeline::init() {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        WHBLogPrintf("audio: SDL_Init(AUDIO): %s", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want{}, got{};
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = CHANNELS;
    want.samples  = 1024;
    want.callback = sdl_audio_cb;
    want.userdata = this;

    sdl_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (!sdl_dev_) {
        WHBLogPrintf("audio: SDL_OpenAudioDevice: %s", SDL_GetError());
        return false;
    }

    pcm_buf_.assign(PCM_RING_CAP, 0);
    SDL_PauseAudioDevice(sdl_dev_, 0);  // start in running state (silent until decode)

    cache_stop_.store(false);
    cache_thread_ = std::thread(&AudioPipeline::cache_cleanup_fn, this);

    WHBLogPrint("audio: device opened");
    return true;
}

void AudioPipeline::shutdown() {
    cache_stop_.store(true);
    if (cache_thread_.joinable()) cache_thread_.join();

    stop();
    ax_shutdown();
    if (sdl_dev_) { SDL_CloseAudioDevice(sdl_dev_); sdl_dev_ = 0; }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool AudioPipeline::play(const std::string &cdn_url,
                          const std::vector<uint8_t> &aes_key,
                          const std::vector<uint8_t> &file_id,
                          int start_ms) {
    stop();

    cdn_url_  = cdn_url;
    aes_key_  = aes_key;
    file_id_  = file_id;
    seek_ms_.store(start_ms > 0 ? start_ms : -1);
    stop_flag_.store(false);
    paused_.store(false);
    pos_ms_atomic_.store(0);

    {
        std::lock_guard<std::mutex> lk(pcm_mu_);
        pcm_read_ = pcm_write_ = 0;
        crystal_prev_l_ = crystal_prev_r_ = 0;
    }

    decode_thread_ = std::thread(&AudioPipeline::decode_thread_fn, this);
    return true;
}

void AudioPipeline::pause() {
    paused_.store(true);
    SDL_PauseAudioDevice(sdl_dev_, 1);
}

void AudioPipeline::resume() {
    paused_.store(false);
    SDL_PauseAudioDevice(sdl_dev_, 0);
}

void AudioPipeline::seek(int pos_ms) {
    seek_ms_.store(pos_ms);
}

void AudioPipeline::stop() {
    stop_flag_.store(true);
    paused_.store(false);
    if (decode_thread_.joinable()) decode_thread_.join();
    if (vf_open_) { ov_clear(&vf_); vf_open_ = false; }
    playing_.store(false);
    stop_flag_.store(false);
}

void AudioPipeline::get_pcm_snapshot(float *buf, int n) {
    std::lock_guard<std::mutex> lk(pcm_mu_);
    size_t avail = pcm_write_ - pcm_read_;
    size_t want  = (size_t)n * CHANNELS;
    size_t have  = std::min(want, avail) & ~(size_t)(CHANNELS - 1); // align to frame
    size_t base  = pcm_write_ - have;
    int    frames = (int)(have / CHANNELS);
    for (int i = 0; i < n; ++i) {
        if (i < frames) {
            float l = pcm_buf_[(base + i*CHANNELS)   & (PCM_RING_CAP-1)] * (1.0f/32768.0f);
            float r = pcm_buf_[(base + i*CHANNELS+1) & (PCM_RING_CAP-1)] * (1.0f/32768.0f);
            buf[i] = (l + r) * 0.5f;
        } else {
            buf[i] = 0.0f;
        }
    }
}

void AudioPipeline::set_crystalizer(bool enabled, int strength) {
    strength = std::max(1, std::min(25, strength));
    crystal_gain_.store(strength * 25);   // 25–625, interpreted as gain/256
    crystal_enabled_.store(enabled);
}

void AudioPipeline::set_volume(int pct) {
    pct = std::max(0, std::min(100, pct));
    volume_.store(pct);
    float t = pct / 100.0f;
    vol_gain_.store((int)(t * t * 1024.0f + 0.5f));
}


int AudioPipeline::position_ms() const {
    return pos_ms_atomic_.load();
}

// ── Cache retention thread ─────────────────────────────────────────────────────
//
// Runs on CPU0, sweeps /vol/external01/spotify_cache once per hour.
// Deletes chunk files (CACHE_BASE/{file_id_hex}/{chunk_idx}) whose mtime is
// older than 3 days, then removes any track directories that are now empty.
// FAT32 on the SD card reports write-time in st_mtime with 2-second precision.

// Same path as CACHE_BASE above — reuse directly so there's one source of truth.
#define CACHE_GC_BASE CACHE_BASE
static constexpr time_t      CACHE_MAX_AGE = 3 * 24 * 60 * 60;  // 3 days
static constexpr int         CACHE_GC_INTERVAL_SEC = 3600;       // 1 hour

// One row written to cache-state.csv per kept track.
struct CacheRow {
    char     file_id[256];
    long long cached_at;
    long long last_played_at;
    int       fragment_count;
    int64_t   size_bytes;
};

void AudioPipeline::cache_cleanup_fn() {
    OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0);

    // Delay first sweep so startup I/O doesn't compete with app init.
    for (int i = 0; i < 15 && !cache_stop_.load(); ++i)
        OSSleepTicks(OSMillisecondsToTicks(1000));

    while (!cache_stop_.load()) {
        time_t cutoff = time(nullptr) - CACHE_MAX_AGE;
        int evicted_tracks = 0, evicted_chunks = 0;

        std::vector<CacheRow> rows;
        int64_t total_bytes = 0;

        DIR *base = opendir(CACHE_GC_BASE);
        if (base) {
            struct dirent *tde;
            while ((tde = readdir(base)) != nullptr) {
                if (tde->d_name[0] == '.') continue;

                char tdir[512];
                snprintf(tdir, sizeof(tdir), "%s/%s", CACHE_GC_BASE, tde->d_name);

                // Read two-line .timestamp: line 1 = cached_at, line 2 = last_played_at.
                // Directories without one are mid-setup — skip.
                char ts_path[560];
                snprintf(ts_path, sizeof(ts_path), "%s/.timestamp", tdir);
                FILE *tf = fopen(ts_path, "r");
                if (!tf) continue;
                long long cached_at = 0, last_played_at = 0;
                fscanf(tf, "%lld\n%lld", &cached_at, &last_played_at);
                fclose(tf);

                if ((time_t)last_played_at < cutoff) {
                    // Evict: delete all chunk files, then the directory.
                    DIR *td = opendir(tdir);
                    if (td) {
                        struct dirent *cde;
                        while ((cde = readdir(td)) != nullptr) {
                            if (cde->d_name[0] == '.') continue;
                            char path[768];
                            snprintf(path, sizeof(path), "%s/%s", tdir, cde->d_name);
                            unlink(path);
                            ++evicted_chunks;
                        }
                        closedir(td);
                    }
                    unlink(ts_path);
                    rmdir(tdir);
                    ++evicted_tracks;
                    continue;
                }

                // Keep: count fragments and sum actual sizes for the CSV.
                CacheRow row{};
                snprintf(row.file_id, sizeof(row.file_id), "%s", tde->d_name);
                row.cached_at      = cached_at;
                row.last_played_at = last_played_at;

                DIR *td = opendir(tdir);
                if (td) {
                    struct dirent *cde;
                    while ((cde = readdir(td)) != nullptr) {
                        if (cde->d_name[0] == '.') continue;
                        char path[768];
                        snprintf(path, sizeof(path), "%s/%s", tdir, cde->d_name);
                        struct stat st;
                        if (stat(path, &st) == 0) {
                            ++row.fragment_count;
                            row.size_bytes += st.st_size;
                        }
                    }
                    closedir(td);
                }
                total_bytes += row.size_bytes;
                rows.push_back(row);
            }
            closedir(base);
        }

        // Write cache-state.csv with one row per kept track.
        // Ensure the base directory exists even if no tracks have been cached yet.
        mkdir(CACHE_GC_BASE, 0755);
        char csv_path[256];
        snprintf(csv_path, sizeof(csv_path), "%s/cache-state.csv", CACHE_GC_BASE);
        FILE *csv = fopen(csv_path, "w");
        if (csv) {
            fprintf(csv, "file_id,cached_at,last_played_at,fragment_count,size_bytes\n");
            for (const auto &row : rows)
                fprintf(csv, "%s,%lld,%lld,%d,%lld\n",
                        row.file_id, row.cached_at, row.last_played_at,
                        row.fragment_count, (long long)row.size_bytes);
            fclose(csv);
        }

        cache_total_bytes_.store((int32_t)(total_bytes / 1024));

        if (evicted_tracks)
            WHBLogPrintf("cache: evicted %d tracks (%d chunks) not played in 3 days",
                         evicted_tracks, evicted_chunks);
        WHBLogPrintf("cache: %zu tracks, %.1f MB total",
                     rows.size(), total_bytes / (1024.0 * 1024.0));

        for (int i = 0; i < CACHE_GC_INTERVAL_SEC && !cache_stop_.load(); ++i)
            OSSleepTicks(OSMillisecondsToTicks(1000));
    }
}

} // namespace Connect
