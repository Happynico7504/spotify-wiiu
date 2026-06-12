#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <SDL2/SDL.h>

// Tremor — integer Vorbis decoder (no FPU required, suits PowerPC Broadway)
#ifdef __cplusplus
extern "C" {
#endif
#include <tremor/ivorbisfile.h>
#ifdef __cplusplus
}
#endif

namespace Connect {

// Decodes a Spotify audio stream. SDL routes PCM to both TV (HDMI) and
// GamePad simultaneously; use the GamePad's hardware volume slider for balance.
//
// Lifecycle:
//   1. set_volume() before or during playback
//   2. play(file_id, key)  → starts fetch + decode pipeline
//   3. seek(ms)            → repositions within current track
//   4. pause() / resume()
//   5. stop()              → flushes and tears down pipeline
//
// The Shannon key is the per-file AES key received from the AP after
// requesting audio file access for the given file_id.

class AudioPipeline {
public:
    explicit AudioPipeline();
    ~AudioPipeline();

    bool init();
    void shutdown();

    // Start playing a track.
    // cdn_url:   resolved HTTPS URL from storage-resolve (via Spirc::resolve_cdn)
    // aes_key:   16-byte AES key from AP AudioKeyRequest response
    // Returns false if setup fails.
    bool play(const std::string &cdn_url,
              const std::vector<uint8_t> &aes_key,
              const std::vector<uint8_t> &file_id,
              int start_ms = 0);

    void pause();
    void resume();
    void seek(int position_ms);
    void stop();

    void set_volume(int pct);           // 0–100, applied in software before output
    void set_crystalizer(bool enabled, int strength);  // strength 1–10

    // Copy the most recent n mono samples (stereo L+R averaged, normalised
    // to [-1,1]) into buf.  Fills with zeros if fewer samples are available.
    // Safe to call from any thread.
    void get_pcm_snapshot(float *buf, int n);

    bool    is_playing() const { return playing_.load(); }
    int     position_ms() const;
    // Returns total bytes currently in the SD chunk cache (-1 until first GC sweep).
    // Stored internally as KB (int32_t) to avoid 8-byte atomics on Broadway (PPC32).
    int64_t cache_total_bytes() const {
        int32_t kb = cache_total_bytes_.load();
        return kb < 0 ? -1 : (int64_t)kb * 1024;
    }

    // Called when the track finishes naturally (not on stop/seek)
    std::function<void()> on_track_end;

private:
    // ── decode thread ────────────────────────────────────────────────────────
    void decode_thread_fn();

    // ── audio output ─────────────────────────────────────────────────────────
    // SDL_audio callback fills from pcm_buf_
    static void sdl_audio_cb(void *userdata, uint8_t *stream, int len);
    void fill_audio(uint8_t *stream, int len);

    // Wii U AX DRC output (separate from SDL_audio which targets TV/HDMI).
    // Implemented in audio_ax.cpp — stub returns immediately until AX is wired.
    bool  ax_init();
    void  ax_shutdown();
    void  ax_push(const int16_t *pcm, size_t frames);

    // ── state ─────────────────────────────────────────────────────────────────
    std::string           cdn_url_;
    std::vector<uint8_t>  aes_key_;    // 16 bytes, AES-128
    std::vector<uint8_t>  file_id_;    // 20 bytes, SD cache key

    SDL_AudioDeviceID     sdl_dev_ = 0;
    std::atomic<int>      volume_{100};
    std::atomic<int>      vol_gain_{1024};      // (vol/100)^2 * 1024, fixed-point
    std::atomic<bool>     crystal_enabled_{false};
    std::atomic<int>      crystal_gain_{0};    // strength * 25 → 0–250 (>>8 fixed-point)
    // Owned by fill_audio (SDL audio thread only); reset under pcm_mu_ in play().
    int16_t crystal_prev_l_ = 0, crystal_prev_r_ = 0;
    std::atomic<bool>     playing_{false};
    std::atomic<bool>     paused_{false};
    std::atomic<int>      seek_ms_{-1};  // -1 = no pending seek
    std::atomic<bool>     stop_flag_{false};
    std::atomic<int>      pos_ms_atomic_{0};  // updated by decode thread

    // PCM ring buffer — indices grow monotonically; access via & (cap-1)
    // Written by decode thread, read by SDL audio callback.
    std::mutex            pcm_mu_;
    std::vector<int16_t>  pcm_buf_;
    size_t                pcm_read_  = 0;
    size_t                pcm_write_ = 0;

    std::thread           decode_thread_;

    // Vorbis state (owned by decode thread only)
    OggVorbis_File        vf_{};
    bool                  vf_open_ = false;

    // ── cache retention thread (CPU0, hourly sweep, configurable TTL) ────────
    void cache_cleanup_fn();
    std::thread            cache_thread_;
    std::atomic<bool>      cache_stop_{false};
    std::atomic<int32_t>   cache_total_bytes_{-1};  // KB; -1 until first sweep
};

} // namespace Connect
