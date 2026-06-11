#include "spirc.h"
#include "ap.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <thread>

#include <curl/curl.h>
#include <mbedtls/sha1.h>
#include <whb/log.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include "cJSON/cJSON.h"

namespace Connect {

// ── MessageType enum (from spirc.proto, values are 0x-prefixed in proto) ──────
namespace MsgType {
    constexpr uint32_t Hello     = 0x01;  // kMessageTypeHello
    constexpr uint32_t Goodbye   = 0x02;  // kMessageTypeGoodbye
    constexpr uint32_t Probe     = 0x03;  // kMessageTypeProbe
    constexpr uint32_t Notify    = 0x0a;  // kMessageTypeNotify
    constexpr uint32_t Load      = 0x14;  // kMessageTypeLoad
    constexpr uint32_t Play      = 0x15;  // kMessageTypePlay
    constexpr uint32_t Pause     = 0x16;  // kMessageTypePause
    constexpr uint32_t PlayPause = 0x17;  // kMessageTypePlayPause
    constexpr uint32_t Seek      = 0x18;  // kMessageTypeSeek
    constexpr uint32_t Prev      = 0x19;  // kMessageTypePrev
    constexpr uint32_t Next      = 0x1a;  // kMessageTypeNext
    constexpr uint32_t Volume    = 0x1b;  // kMessageTypeVolume
}

// ── Protobuf helpers ──────────────────────────────────────────────────────────

static void pb_vi(std::vector<uint8_t> &b, uint64_t v) {
    do {
        uint8_t byte = v & 0x7F; v >>= 7;
        if (v) byte |= 0x80;
        b.push_back(byte);
    } while (v);
}
static void pb_tag(std::vector<uint8_t> &b, uint32_t f, uint8_t w) {
    pb_vi(b, ((uint64_t)f << 3) | w);
}
static void pb_u32(std::vector<uint8_t> &b, uint32_t f, uint32_t v) {
    pb_tag(b, f, 0); pb_vi(b, v);
}
static void pb_i64(std::vector<uint8_t> &b, uint32_t f, int64_t v) {
    pb_tag(b, f, 0); pb_vi(b, (uint64_t)v);
}
static void pb_bool(std::vector<uint8_t> &b, uint32_t f, bool v) {
    pb_tag(b, f, 0); pb_vi(b, v ? 1u : 0u);
}
static void pb_bytes(std::vector<uint8_t> &b, uint32_t f, const uint8_t *p, size_t n) {
    pb_tag(b, f, 2); pb_vi(b, n); b.insert(b.end(), p, p + n);
}
static void pb_str(std::vector<uint8_t> &b, uint32_t f, const std::string &s) {
    pb_bytes(b, f, reinterpret_cast<const uint8_t *>(s.data()), s.size());
}
static void pb_msg(std::vector<uint8_t> &b, uint32_t f, const std::vector<uint8_t> &m) {
    pb_bytes(b, f, m.data(), m.size());
}
static void pb_f64(std::vector<uint8_t> &b, uint32_t f, double v) {
    pb_tag(b, f, 1);  // wire type 1 = 64-bit fixed
    uint64_t bits; memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; ++i) b.push_back((uint8_t)((bits >> (i * 8)) & 0xFF));
}
static void pb_sint32(std::vector<uint8_t> &b, uint32_t f, int32_t v) {
    pb_tag(b, f, 0);
    // zigzag encoding required for sint32
    pb_vi(b, ((uint64_t)(uint32_t)(v << 1)) ^ (uint64_t)(uint32_t)(v >> 31));
}

// ── Protobuf reader ───────────────────────────────────────────────────────────

struct PRd {
    const uint8_t *p, *end;
    bool ok = true;

    bool vi(uint64_t &out) {
        out = 0; int sh = 0;
        while (p < end) {
            uint8_t b = *p++;
            out |= (uint64_t)(b & 0x7F) << sh; sh += 7;
            if (!(b & 0x80)) return true;
        }
        return ok = false;
    }
    bool next(uint32_t &field, uint8_t &wire) {
        if (p >= end) return false;
        uint64_t v; if (!vi(v)) return false;
        field = (uint32_t)(v >> 3); wire = v & 7; return true;
    }
    bool skip(uint8_t w) {
        uint64_t n;
        switch (w) {
            case 0: return vi(n);
            case 1: if (p+8>end){ok=false;return false;} p+=8; return true;
            case 2: if (!vi(n)||p+(size_t)n>end){ok=false;return false;} p+=(size_t)n; return true;
            case 5: if (p+4>end){ok=false;return false;} p+=4; return true;
            default: return ok=false;
        }
    }
    bool enter(PRd &sub) {
        uint64_t n; if (!vi(n)||p+(size_t)n>end) return ok=false;
        sub = {p, p+(size_t)n, true}; p += (size_t)n; return true;
    }
    bool read_bytes(std::vector<uint8_t> &out) {
        uint64_t n; if (!vi(n)||p+(size_t)n>end) return ok=false;
        out.assign(p, p+(size_t)n); p += (size_t)n; return true;
    }
    bool read_str(std::string &out) {
        uint64_t n; if (!vi(n)||p+(size_t)n>end) return ok=false;
        out.assign(reinterpret_cast<const char*>(p), (size_t)n);
        p += (size_t)n; return true;
    }
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string hex_encode(const std::vector<uint8_t> &b) {
    static const char hx[] = "0123456789abcdef";
    std::string s; s.reserve(b.size() * 2);
    for (uint8_t v : b) { s += hx[v >> 4]; s += hx[v & 0xF]; }
    return s;
}

// Convert a Spotify base-62 track ID to its 16-byte GID.
static std::vector<uint8_t> base62_to_gid(const char *s, size_t len) {
    static const char alpha[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::vector<uint8_t> r(16, 0);
    for (size_t i = 0; i < len; ++i) {
        const char *pos = strchr(alpha, s[i]);
        if (!pos) return {};
        int digit = (int)(pos - alpha);
        int carry = digit;
        for (int j = 15; j >= 0; --j) {
            int v = r[j] * 62 + carry;
            r[j] = (uint8_t)(v & 0xFF);
            carry = v >> 8;
        }
        if (carry) return {};
    }
    return r;
}

// Convert a 16-byte GID to a Spotify base-62 track ID (22 characters).
static std::string gid_to_base62(const std::vector<uint8_t> &gid) {
    static const char alpha[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint8_t tmp[16]; memcpy(tmp, gid.data(), 16);
    char out[23]; out[22] = '\0';
    for (int i = 21; i >= 0; i--) {
        int rem = 0;
        for (int j = 0; j < 16; j++) {
            int v = rem * 256 + tmp[j];
            tmp[j] = (uint8_t)(v / 62);
            rem = v % 62;
        }
        out[i] = alpha[rem];
    }
    return std::string(out, 22);
}

static std::vector<uint8_t> b64_decode(const char *s, size_t len) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::vector<uint8_t> out;
    out.reserve(len * 3 / 4);
    int val = 0, bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = T[(unsigned char)s[i]];
        if (v < 0) continue;
        val = (val << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(val >> bits)); val &= (1 << bits) - 1; }
    }
    return out;
}

// ── ClusterUpdate parser ──────────────────────────────────────────────────────
// Parses a connect-state v1 ClusterUpdate proto (field 1 = Cluster) and
// extracts the active device id and its current PlayerState track URI.
// Field numbers from connect.proto / player.proto (librespot-compatible):
//   ClusterUpdate.cluster       = 1
//   Cluster.active_device_id    = 2
//   Cluster.player_state        = 3
//   PlayerState.context_uri     = 2
//   PlayerState.track           = 7  (ProvidedTrack, current track)
//   PlayerState.prev_tracks     = 8  (repeated ProvidedTrack)
//   PlayerState.next_tracks     = 9  (repeated ProvidedTrack)
//   PlayerState.position_as_of_timestamp = 10
//   PlayerState.is_playing      = 12
//   ProvidedTrack.uri           = 1
//   ProvidedTrack.uid           = 2

struct ClusterState {
    std::string              active_device_id;
    std::string              track_uri;
    std::string              track_uid;
    std::string              context_uri;
    int64_t                  position_ms         = 0;
    bool                     is_playing          = false;
    uint32_t                 context_index_track = UINT32_MAX;  // UINT32_MAX = not present
    std::vector<std::string> prev_uris;
    std::vector<std::string> prev_uids;
    std::vector<std::string> next_uris;
    std::vector<std::string> next_uids;
};

static ClusterState parse_cluster_update(const std::vector<uint8_t> &data) {
    ClusterState cs;
    if (data.empty()) return cs;

    PRd top{data.data(), data.data() + data.size()};
    uint32_t f; uint8_t w;
    while (top.next(f, w)) {
        if (f == 1 && w == 2) {                        // cluster
            PRd cl; top.enter(cl);
            while (cl.next(f, w)) {
                if (f == 2 && w == 2) {                // active_device_id
                    cl.read_str(cs.active_device_id);
                } else if (f == 3 && w == 2) {         // player_state
                    PRd ps; cl.enter(ps);
                    while (ps.next(f, w)) {
                        if (f == 2 && w == 2) {        // context_uri
                            ps.read_str(cs.context_uri);
                        } else if (f == 6 && w == 2) { // ContextIndex (absolute playlist pos)
                            PRd ci; ps.enter(ci);
                            while (ci.next(f, w)) {
                                if (f == 2 && w == 0) {
                                    uint64_t v; ci.vi(v);
                                    cs.context_index_track = (uint32_t)v;
                                } else ci.skip(w);
                            }
                        } else if (f == 7 && w == 2) { // track (ProvidedTrack)
                            PRd tr; ps.enter(tr);
                            while (tr.next(f, w)) {
                                if      (f == 1 && w == 2) tr.read_str(cs.track_uri);
                                else if (f == 2 && w == 2) tr.read_str(cs.track_uid);
                                else                        tr.skip(w);
                            }
                        } else if (f == 8 && w == 2) { // prev_tracks (repeated ProvidedTrack)
                            PRd tr; ps.enter(tr);
                            std::string uri, uid;
                            while (tr.next(f, w)) {
                                if      (f == 1 && w == 2) tr.read_str(uri);
                                else if (f == 2 && w == 2) tr.read_str(uid);
                                else                        tr.skip(w);
                            }
                            if (!uri.empty()) {
                                cs.prev_uris.push_back(uri);
                                cs.prev_uids.push_back(uid);
                            }
                        } else if (f == 9 && w == 2) { // next_tracks (repeated ProvidedTrack)
                            PRd tr; ps.enter(tr);
                            std::string uri, uid;
                            while (tr.next(f, w)) {
                                if      (f == 1 && w == 2) tr.read_str(uri);
                                else if (f == 2 && w == 2) tr.read_str(uid);
                                else                        tr.skip(w);
                            }
                            if (!uri.empty()) {
                                cs.next_uris.push_back(uri);
                                cs.next_uids.push_back(uid);
                            }
                        } else if (f == 10 && w == 0) { // position_as_of_timestamp
                            uint64_t v; ps.vi(v); cs.position_ms = (int64_t)v;
                        } else if (f == 12 && w == 0) { // is_playing
                            uint64_t v; ps.vi(v); cs.is_playing = (v != 0);
                        } else {
                            ps.skip(w);
                        }
                    }
                } else {
                    cl.skip(w);
                }
            }
        } else {
            top.skip(w);
        }
    }
    return cs;
}

// Forward declarations for static helpers defined later in this file.
static std::string get_access_token(const std::string &username,
                                     const std::vector<uint8_t> &auth_data,
                                     const std::string &device_id);
static std::string resolve_spclient_host();

// ── Constructor / destructor ──────────────────────────────────────────────────

Spirc::Spirc(AP *ap, const std::string &username,
             const std::string &device_name, const std::string &device_id)
    : ap_(ap), username_(username),
      device_name_(device_name), device_id_(device_id)
{}

Spirc::~Spirc() { stop(); }

// ── start / stop ──────────────────────────────────────────────────────────────

bool Spirc::start(Callbacks cb) {
    callbacks_ = std::move(cb);
    srand((unsigned)time(nullptr));

    std::string user_uri = "hm://remote/3/user/" + username_ + "/";

    // Modern Spotify requires these subscriptions to stay connected.
    mercury_subscribe("hm://pusher/v1/connections/");
    mercury_subscribe("hm://connect-state/v1/cluster");
    // Volume changes from host app arrive here as binary SetVolumeCommand proto.
    mercury_subscribe("hm://connect-state/v1/connect/volume");
    // Old Spirc endpoints for legacy command dispatch.
    mercury_subscribe(user_uri);
    mercury_subscribe(user_uri + device_id_ + "/");

    started_.store(true);
    send_hello();
    WHBLogPrintf("spirc: started, sub=%s", user_uri.c_str());

    // Start Dealer WebSocket in background.
    // Resolve the spclient first so the dealer is in the SAME cluster — Spotify's
    // spclient does a local-only lookup for Dealer sessions, so a cross-cluster
    // mismatch causes HTTP 404 on every PutStateRequest.
    dealer_init_thread_ = std::thread([this]() {
        OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0);
        std::string atk;
        { std::lock_guard<std::mutex> lk(token_mu_); atk = access_token_; }
        if (atk.empty()) {
            atk = get_access_token(username_, ap_->reusable_creds(), device_id_);
            if (atk.empty()) { WHBLogPrint("dealer: no access token"); return; }
            std::lock_guard<std::mutex> lk(token_mu_);
            access_token_ = atk;
        }
        if (!started_.load()) return;

        // Resolve spclient and cache it (saves a second resolve in put_connect_state_sync).
        std::string sp = resolve_spclient_host();
        { std::lock_guard<std::mutex> lk(spclient_mu_); if (spclient_host_.empty()) spclient_host_ = sp; }

        // Derive dealer host from spclient host by replacing "-spclient." with "-dealer.".
        // e.g.  gew4-spclient.spotify.com  →  gew4-dealer.spotify.com
        std::string dealer_host = "dealer.spotify.com";   // fallback
        {
            const std::string from = "-spclient.";
            const std::string to   = "-dealer.";
            auto p = sp.find(from);
            if (p != std::string::npos) dealer_host = sp.substr(0, p) + to + sp.substr(p + from.size());
        }
        WHBLogPrintf("spirc: dealer host: %s", dealer_host.c_str());

        std::string ws_url = "wss://" + dealer_host + "/?access_token=" + atk;
        dealer_.start(ws_url,
            [this](const std::string &cid) {
                { std::lock_guard<std::mutex> lk(conn_mu_); connection_id_ = cid; }
                WHBLogPrintf("spirc: dealer connection_id ready (%zu chars)", cid.size());
                if (started_.load())
                    put_connect_state_async(1 /* SPIRC_HELLO */, playing_, pos_ms_, vol_pct_);
            },
            [this](const std::string &uri, const std::vector<uint8_t> &payload) {
                handle_dealer_message(uri, payload);
            },
            [this](const std::string &ident, const std::string &cmd_json) {
                return handle_dealer_request(ident, cmd_json);
            });
    });

    return true;
}

void Spirc::stop() {
    if (!started_.exchange(false)) return;
    dealer_.stop();
    if (dealer_init_thread_.joinable()) dealer_init_thread_.join();
    // Send Goodbye frame
    std::string user_uri = "hm://remote/3/user/" + username_ + "/";
    auto frame = build_frame(MsgType::Goodbye, false, 0, vol_pct_);
    mercury_send(user_uri, frame);
}

// ── Mercury wire helpers ──────────────────────────────────────────────────────

// Build and send a single-part or two-part Mercury packet.
// Returns the mercury sequence number used.
static uint32_t make_mercury_pkt(std::vector<uint8_t> &out,
                                  uint32_t seq,
                                  const std::vector<uint8_t> &header,
                                  const std::vector<uint8_t> &body) {
    // Mercury wire format: [2B seq_len] [seq_len bytes seq] [1B flags] [2B part_count]
    uint16_t parts = body.empty() ? 1 : 2;
    out.push_back(0x00); out.push_back(0x04);  // seq_len = 4 bytes
    out.push_back((seq >> 24) & 0xFF);
    out.push_back((seq >> 16) & 0xFF);
    out.push_back((seq >>  8) & 0xFF);
    out.push_back( seq        & 0xFF);
    out.push_back(0x01); // flags: complete
    out.push_back((parts >> 8) & 0xFF);
    out.push_back( parts       & 0xFF);

    uint16_t hlen = (uint16_t)header.size();
    out.push_back((hlen >> 8) & 0xFF);
    out.push_back( hlen       & 0xFF);
    out.insert(out.end(), header.begin(), header.end());

    if (!body.empty()) {
        uint16_t blen = (uint16_t)body.size();
        out.push_back((blen >> 8) & 0xFF);
        out.push_back( blen       & 0xFF);
        out.insert(out.end(), body.begin(), body.end());
    }
    return seq;
}

void Spirc::mercury_subscribe(const std::string &uri) {
    std::vector<uint8_t> hdr;
    pb_str(hdr, 1, uri);
    pb_str(hdr, 3, "SUB");

    uint32_t seq;
    { std::lock_guard<std::mutex> lk(seq_mu_); seq = mercury_seq_++; }

    std::vector<uint8_t> pkt;
    make_mercury_pkt(pkt, seq, hdr, {});
    ap_->send_packet(Cmd::MercurySub, pkt);
}

void Spirc::mercury_send(const std::string &uri, const std::vector<uint8_t> &frame) {
    std::vector<uint8_t> hdr;
    pb_str(hdr, 1, uri);
    pb_str(hdr, 3, "SEND");

    uint32_t seq;
    { std::lock_guard<std::mutex> lk(seq_mu_); seq = mercury_seq_++; }

    std::vector<uint8_t> pkt;
    make_mercury_pkt(pkt, seq, hdr, frame);
    ap_->send_packet(Cmd::MercuryReq, pkt);
}

void Spirc::mercury_get(const std::string &uri,
    std::function<void(bool, std::vector<std::vector<uint8_t>>)> cb)
{
    std::vector<uint8_t> hdr;
    pb_str(hdr, 1, uri);
    pb_str(hdr, 3, "GET");

    uint32_t seq;
    { std::lock_guard<std::mutex> lk(seq_mu_); seq = mercury_seq_++; }

    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_[seq] = {std::move(cb)};
    }

    std::vector<uint8_t> pkt;
    make_mercury_pkt(pkt, seq, hdr, {});
    ap_->send_packet(Cmd::MercuryReq, pkt);
}

// ── Frame builders ────────────────────────────────────────────────────────────

std::vector<uint8_t> Spirc::build_device_state(bool active, int vol_pct) {
    uint32_t vol = (uint32_t)(vol_pct * 655); // 0-100 → 0-65535

    // DeviceState field numbers from spirc.proto (hex values):
    //   sw_version=0x1=1  is_active=0xa=10  can_play=0xb=11
    //   volume=0xc=12  name=0xd=13  capabilities=0x11=17
    // CapabilityType enum: kGaiaEqConnectId=0x5=5  kIsObservable=0x7=7
    //   kVolumeSteps=0x8=8  kSupportedTypes=0x9=9  kCommandAcks=0xa=10
    auto cap = [](uint32_t t,
                  const std::initializer_list<int32_t> &ints,
                  const std::initializer_list<std::string> &strs) {
        std::vector<uint8_t> c;
        pb_u32(c, 1, t);
        for (int32_t v : ints) pb_sint32(c, 2, v);
        for (const auto &s : strs) pb_str(c, 3, s);
        return c;
    };

    std::vector<uint8_t> ds;
    pb_str(ds,  1,  "1.0.0");      // sw_version
    pb_bool(ds, 10, active);        // is_active
    pb_bool(ds, 11, true);          // can_play
    pb_u32(ds,  12, vol);           // volume
    pb_str(ds,  13, device_name_); // name

    pb_msg(ds, 17, cap(8,  {64},   {}));                  // kVolumeSteps=0x8
    pb_msg(ds, 17, cap(9,  {},     {"audio/track"}));      // kSupportedTypes=0x9
    pb_msg(ds, 17, cap(7,  {1},    {}));                  // kIsObservable=0x7
    pb_msg(ds, 17, cap(10, {1},    {}));                  // kCommandAcks=0xa
    pb_msg(ds, 17, cap(5,  {1},    {}));                  // kGaiaEqConnectId=0x5
    return ds;
}


// Decode percent-encoded characters in a URI component.

// ── connect-state v1 builders ─────────────────────────────────────────────────

std::vector<uint8_t> Spirc::build_player_state(bool playing, int pos_ms, int64_t now_ms) {
    std::vector<uint8_t> ps;
    pb_i64(ps, 1, now_ms);                          // timestamp
    // Resolve the context URI to send.  Prefer the stored context if it is a
    // type the connect-state API accepts.  Fall back to the current track URI
    // so we never send is_active=true with an absent context_uri (HTTP 500).
    // "spotify:user:..." URIs cause INVALID_ENTITY (400) — omit them entirely.
    std::string ctx_uri;
    if (!context_uri_.empty() &&
        (context_uri_.compare(0, 17, "spotify:playlist:") == 0 ||
         context_uri_.compare(0, 14, "spotify:album:")    == 0 ||
         context_uri_.compare(0, 15, "spotify:artist:")   == 0 ||
         context_uri_.compare(0, 20, "spotify:collection:")== 0 ||
         context_uri_.compare(0, 13, "spotify:show:")     == 0 ||
         context_uri_.compare(0, 14, "spotify:track:")    == 0)) {
        ctx_uri = context_uri_;
    }
    // ProvidedTrack (field 7)
    size_t i = playing_track_idx_;
    // Resolve URI: use stored value, or derive from GID if still empty.
    std::string track_uri_for_ps;
    if (i < track_uris_.size() && !track_uris_[i].empty()) {
        track_uri_for_ps = track_uris_[i];
    } else if (i < track_gids_.size() && track_gids_[i].size() == 16) {
        track_uri_for_ps = "spotify:track:" + gid_to_base62(track_gids_[i]);
        if (i >= track_uris_.size()) track_uris_.resize(i + 1);
        track_uris_[i] = track_uri_for_ps;
    }
    // Fall back: if no accepted context, use the current track URI as a
    // single-track context so we never emit is_active=true with no context_uri.
    if (ctx_uri.empty() && !track_uri_for_ps.empty())
        ctx_uri = track_uri_for_ps;
    if (!ctx_uri.empty()) pb_str(ps, 2, ctx_uri);
    // Build a ProvidedTrack proto for context slot idx.
    // ProvidedTrack field numbers (player.proto): uri=1 uid=2 metadata=3 provider=6
    // metadata map entries use MapEntry encoding: key=1, value=2 inside a LEN field.
    // Required metadata keys (from librespot state/metadata.rs):
    //   "context_uri"   — the enclosing playlist / album URI
    //   "entity_uri"    — same as context_uri for regular context tracks
    //   "context_index" — absolute zero-based position in the context as a decimal string
    auto make_ctx_track = [&](int idx) -> std::vector<uint8_t> {
        std::vector<uint8_t> tr;
        pb_str(tr, 1, track_uris_[idx]);
        if (idx < (int)track_uids_.size() && !track_uids_[idx].empty())
            pb_str(tr, 2, track_uids_[idx]);
        if (!ctx_uri.empty()) {
            auto add_meta = [&](const std::string &mk, const std::string &mv) {
                std::vector<uint8_t> m; pb_str(m, 1, mk); pb_str(m, 2, mv);
                pb_msg(tr, 3, m);
            };
            add_meta("context_uri",  ctx_uri);
            add_meta("entity_uri",   ctx_uri);
            char ibuf[20]; snprintf(ibuf, sizeof(ibuf), "%d", idx);
            add_meta("context_index", ibuf);
        }
        pb_str(tr, 6, "context");
        return tr;
    };

    // current track (field 7)
    if (!track_uri_for_ps.empty())
        pb_msg(ps, 7, make_ctx_track((int)i));

    // prev_tracks (field 8): up to 10 tracks before the current one (librespot limit)
    {
        int start = std::max(0, (int)i - 10);
        for (int k = start; k < (int)i; ++k) {
            if (track_uris_[k].empty()) continue;
            pb_msg(ps, 8, make_ctx_track(k));
        }
    }

    // next_tracks (field 9): up to 80 tracks after the current one (librespot limit).
    // With repeat_context, wraps around the playlist inserting a "spotify:delimiter"
    // sentinel between iterations — mirrors librespot's fill_up_next_tracks().
    {
        int n      = (int)track_uris_.size();
        int filled = 0;
        int k      = (int)i + 1;
        int iter   = 0;
        while (filled < 80) {
            if (k >= n) {
                if (!repeat_) break;
                // Delimiter: hidden invisible sentinel separating playlist loops
                std::vector<uint8_t> d;
                pb_str(d, 1, "spotify:delimiter");
                char ub[24]; snprintf(ub, sizeof(ub), "delimiter%d", iter);
                pb_str(d, 2, ub);
                {
                    auto add_dm = [&](const std::string &mk, const std::string &mv) {
                        std::vector<uint8_t> m; pb_str(m, 1, mk); pb_str(m, 2, mv);
                        pb_msg(d, 3, m);
                    };
                    add_dm("hidden", "true");
                    char ib[20]; snprintf(ib, sizeof(ib), "%d", iter);
                    add_dm("iteration", ib);
                }
                pb_str(d, 6, "context");
                pb_msg(ps, 9, d);
                filled++;
                iter++;
                k = 0;
                continue;
            }
            if (!track_uris_[k].empty()) {
                pb_msg(ps, 9, make_ctx_track(k));
                filled++;
            }
            k++;
        }
    }
    WHBLogPrintf("spirc: ps ctx='%.40s' uri='%.40s' idx=%u dur=%lld play=%d pos=%d",
                 ctx_uri.c_str(), track_uri_for_ps.c_str(),
                 (unsigned)playing_track_idx_, (long long)duration_ms_, (int)playing, pos_ms);
    pb_i64(ps, 10, (int64_t)(pos_ms > 0 ? pos_ms : 0));         // position_as_of_timestamp
    if (duration_ms_ > 0) pb_i64(ps, 11, duration_ms_);         // duration
    pb_bool(ps, 12, playing);                                     // is_playing
    pb_bool(ps, 13, !playing && started_playing_at_ms_ > 0);     // is_paused
    pb_bool(ps, 14, false);                                       // is_buffering
    // ContextPlayerOptions (field 16)
    {
        std::vector<uint8_t> opts;
        pb_bool(opts, 1, shuffle_);        // shuffling_context
        pb_bool(opts, 2, repeat_);         // repeating_context
        pb_bool(opts, 3, repeat_track_);   // repeating_track
        pb_msg(ps, 16, opts);
    }
    // ContextIndex (field 6): tells Spotify our absolute position in the context
    // so the host app's playlist/queue view stays in sync with local skips.
    if (!ctx_uri.empty()) {
        std::vector<uint8_t> cidx;
        pb_u32(cidx, 1, 0);                         // page 0
        pb_u32(cidx, 2, (uint32_t)abs_track_idx_);  // absolute track index
        pb_msg(ps, 6, cidx);
    }
    // playback_speed = 1.0 (field 23): required by some Spotify clients to
    // interpolate the seek bar position between state pushes.
    if (playing) pb_f64(ps, 23, 1.0);
    // has_context = true (field 27): signals the host app that context/queue info is valid.
    if (!ctx_uri.empty()) pb_bool(ps, 27, true);
    return ps;
}

std::vector<uint8_t> Spirc::build_device_info_cs(int vol_pct) {
    uint32_t vol = (uint32_t)(vol_pct * 655);  // 0-100 → 0-65535

    // Capabilities — field numbers from connect.proto (Spotify 1.2.52.442 / librespot 0.8.0)
    // connect.proto Capabilities message: fields 1, 4, 24 are reserved.
    std::vector<uint8_t> caps;
    pb_bool(caps,  2, true);   // can_be_player
    pb_bool(caps,  5, true);   // gaia_eq_connect_id
    pb_bool(caps,  7, true);   // is_observable
    pb_u32 (caps,  8, 64);     // volume_steps (int32, 64 steps)
    pb_str (caps,  9, "audio/track");  // supported_types
    pb_bool(caps, 10, true);   // command_acks
    pb_bool(caps, 16, true);   // is_controllable
    pb_bool(caps, 19, true);   // supports_transfer_command
    pb_bool(caps, 20, true);   // supports_command_request
    pb_bool(caps, 22, true);   // needs_full_player_state
    pb_bool(caps, 25, true);   // supports_set_options_command (shuffle/repeat/volume)

    // DeviceInfo — field numbers from connect.proto
    std::vector<uint8_t> di;
    pb_bool(di, 1,  true);           // can_play
    pb_u32 (di, 2,  vol);            // volume
    pb_str (di, 3,  device_name_);  // name
    pb_msg (di, 4,  caps);           // capabilities
    pb_str (di, 6,  "1.0.0");        // device_software_version
    pb_u32 (di, 7,  9);              // device_type: GAME_CONSOLE = 9
    pb_str (di, 9,  "3.2.6");        // spirc_version
    pb_str (di, 10, device_id_);    // device_id
    pb_str (di, 13, "65b708073fc0480ea92a077233ca87bd");  // client_id
    return di;
}

// ── PutStateRequest ───────────────────────────────────────────────────────────

static int64_t now_ms_wiiu() {
    return (int64_t)(OSGetSystemTick() / OSMillisecondsToTicks(1));
}

void Spirc::put_connect_state_async(uint32_t reason, bool playing, int pos_ms, int vol_pct) {
    // At most one PUT inflight at a time.  If one is already running, mark dirty
    // and return — the inflight thread will fire a follow-up when it finishes.
    int expected = 0;
    if (!push_inflight_.compare_exchange_strong(expected, 1)) {
        dirty_push_.store(true);
        return;
    }

    // We own the slot — build proto from current state and fire.
    // Throttle tracking uses boot-relative ms (OSGetSystemTick); proto timestamps
    // use Unix ms from time() which is 0 on Wii U but was accepted by Spotify before.
    int64_t throttle_ms = now_ms_wiiu();
    int64_t proto_ms    = (int64_t)time(nullptr) * 1000;
    int64_t sat         = started_playing_at_ms_;

    std::vector<uint8_t> device;
    pb_msg(device, 1, build_device_info_cs(vol_pct));
    pb_msg(device, 2, build_player_state(playing, pos_ms, proto_ms));

    std::vector<uint8_t> psr;
    pb_msg (psr, 2, device);
    pb_u32 (psr, 3, 2);
    pb_bool(psr, 4, playing || !context_uri_.empty());
    pb_u32 (psr, 5, reason);
    if (sat > 0) pb_i64(psr, 9, sat);
    pb_i64 (psr, 12, proto_ms);

    last_state_push_ms_ = throttle_ms;
    std::thread([this, psr = std::move(psr), reason, playing]() mutable {
        OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0);
        put_connect_state_sync(std::move(psr), reason, playing);
        push_inflight_.store(0);
        // Always follow up if state changed while inflight, regardless of HTTP result.
        // Dropping dirty on error caused shuffle/repeat commands to snap back permanently
        // because the correction PUT was never sent when the periodic push got 5xx.
        if (dirty_push_.exchange(false))
            put_connect_state_async(4, playing_, pos_ms_, vol_pct_);
    }).detach();
}

long Spirc::put_connect_state_sync(std::vector<uint8_t> psr, uint32_t reason, bool playing) {
    std::string conn_id;
    { std::lock_guard<std::mutex> lk(conn_mu_); conn_id = connection_id_; }
    if (conn_id.empty()) return 0;

    std::string atk;
    { std::lock_guard<std::mutex> lk(token_mu_); atk = access_token_; }
    if (atk.empty()) {
        atk = get_access_token(username_, ap_->reusable_creds(), device_id_);
        if (atk.empty()) { WHBLogPrint("spirc: put_cs: no token"); return 0; }
        std::lock_guard<std::mutex> lk(token_mu_);
        access_token_ = atk;
    }

    std::string host;
    {
        std::lock_guard<std::mutex> lk(spclient_mu_);
        if (spclient_host_.empty()) {
            spclient_host_ = resolve_spclient_host();
            if (spclient_host_.empty()) spclient_host_ = "gew4-spclient.spotify.com";
        }
        host = spclient_host_;
    }

    // PUT /connect-state/v1/devices/{device_id} — same namespace as /devices/helo.
    // The Dealer connection_id goes in X-Spotify-Connection-Id for cluster routing.
    std::string path     = "/connect-state/v1/devices/" + device_id_;
    std::string url      = "https://" + host + path;
    std::string auth_hdr = "Authorization: Bearer " + atk;
    std::string conn_hdr = "X-Spotify-Connection-Id: " + conn_id;

    // For the very first call: also PUT /devices/helo to register the device in the
    // spclient cluster before the session lookup happens.
    if (reason == 1 /* SPIRC_HELLO */) {
        std::string helo_url = "https://" + host + "/connect-state/v1/devices/helo";
        CURL *hc = curl_easy_init();
        if (hc) {
            struct curl_slist *hh = nullptr;
            hh = curl_slist_append(hh, auth_hdr.c_str());
            hh = curl_slist_append(hh, conn_hdr.c_str());
            hh = curl_slist_append(hh, "Content-Type: application/x-protobuf");
            hh = curl_slist_append(hh, "Accept: application/x-protobuf");
            curl_easy_setopt(hc, CURLOPT_URL,            helo_url.c_str());
            curl_easy_setopt(hc, CURLOPT_HTTPHEADER,     hh);
            curl_easy_setopt(hc, CURLOPT_CUSTOMREQUEST,  "PUT");
            curl_easy_setopt(hc, CURLOPT_POSTFIELDS,     psr.data());
            curl_easy_setopt(hc, CURLOPT_POSTFIELDSIZE,  (long)psr.size());
            curl_easy_setopt(hc, CURLOPT_WRITEFUNCTION,
                +[](char *, size_t s, size_t n, void *) -> size_t { return s * n; });
            curl_easy_setopt(hc, CURLOPT_TIMEOUT,        8L);
            curl_easy_setopt(hc, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(hc, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_perform(hc);
            long hc_code = 0; curl_easy_getinfo(hc, CURLINFO_RESPONSE_CODE, &hc_code);
            curl_slist_free_all(hh); curl_easy_cleanup(hc);
            WHBLogPrintf("spirc: helo → HTTP %ld", hc_code);
        }
    }

    CURL *curl = curl_easy_init();
    if (!curl) return 0;
    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, auth_hdr.c_str());
    hdrs = curl_slist_append(hdrs, conn_hdr.c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/x-protobuf");
    hdrs = curl_slist_append(hdrs, "Accept: application/x-protobuf");
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST,  "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     psr.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)psr.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char *, size_t s, size_t n, void *) -> size_t { return s * n; });
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        8L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    if (reason != 1 /* SPIRC_HELLO */ && !helo_done_.load()) {
        curl_slist_free_all(hdrs); curl_easy_cleanup(curl);
        WHBLogPrint("spirc: put_cs skipped — helo not done yet");
        return 0;
    }

    curl_easy_perform(curl);
    long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs); curl_easy_cleanup(curl);
    WHBLogPrintf("spirc: put_cs reason=%u playing=%d → HTTP %ld", reason, (int)playing, code);

    // Expired token → evict so the next call will re-fetch
    if (code == 401) {
        std::lock_guard<std::mutex> lk(token_mu_);
        access_token_.clear();
        WHBLogPrint("spirc: put_cs 401 — token evicted");
    }

    if (reason == 1 /* SPIRC_HELLO */ && (code == 200 || code == 204)) {
        helo_done_.store(true);
        // Always push current state after HELLO succeeds.
        // A Load/Play command may have raced and been dropped while helo_done_=false,
        // and playing_ is read from a detached thread so skip the racy bool guard.
        put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, playing_, pos_ms_, vol_pct_);
    }
    return code;
}

// ── Dealer message dispatch ───────────────────────────────────────────────────

void Spirc::handle_dealer_message(const std::string &uri,
                                   const std::vector<uint8_t> &payload) {
    if (payload.empty()) return;

    // SetVolumeCommand: binary proto { int32 volume = 1; }
    if (uri == "hm://connect-state/v1/connect/volume") {
        PRd rd{payload.data(), payload.data() + payload.size()};
        uint32_t f; uint8_t w;
        while (rd.next(f, w)) {
            if (f == 1 && w == 0) {
                uint64_t v; rd.vi(v);
                int raw = (int)v;  // 0–65535
                int pct = (int)((uint64_t)raw * 100 / 65535);
                WHBLogPrintf("spirc: volume cmd %d (%d%%)", raw, pct);
                vol_pct_ = pct;
                if (callbacks_.on_volume) callbacks_.on_volume(pct);
                put_connect_state_async(5 /* VOLUME_CHANGED */, playing_, pos_ms_, pct);
            } else rd.skip(w);
        }
        return;
    }

    static constexpr char CLUSTER[] = "hm://connect-state/v1/cluster";
    if (uri.compare(0, sizeof(CLUSTER) - 1, CLUSTER) != 0) return;

    ClusterState cs = parse_cluster_update(payload);
    WHBLogPrintf("spirc: cluster upd active='%.20s' track='%.60s' playing=%d",
                 cs.active_device_id.c_str(), cs.track_uri.c_str(), (int)cs.is_playing);

    if (cs.active_device_id != device_id_) {
        WHBLogPrintf("spirc: cluster skip: active='%.40s' ours='%.40s'",
                     cs.active_device_id.c_str(), device_id_.c_str());
        return;
    }
    if (cs.track_uri.empty()) return;

    // Suppress cluster echoes of our own state.
    // Allow through only when Spotify signals a NEW track or wants to REPLAY
    // the current track (single-track repeat: playing_=false, cs.is_playing=true).
    if (cs.track_uri == current_track_uri_ && (playing_ || !cs.is_playing)) {
        // Echo: same track, same playing state (or we're already playing it).
        // Periodic keepalive PUT if we haven't pushed recently.
        int64_t now_ms = now_ms_wiiu();
        if (helo_done_.load() && now_ms - last_state_push_ms_ >= 30000)
            put_connect_state_async(4, playing_, pos_ms_, vol_pct_);
        return;
    }
    // Suppress cluster updates for old tracks while we're loading/playing a new one.
    // This drops echoes of PUT(old_track) that complete after we already switched:
    // those echoes have cs.track_uri == old_track != current_track_uri_.
    if (!current_track_uri_.empty() && cs.track_uri != current_track_uri_ && playing_) {
        WHBLogPrintf("spirc: cluster drop stale echo '%s' (current='%s')",
                     cs.track_uri.c_str(), current_track_uri_.c_str());
        return;
    }

    WHBLogPrintf("spirc: dealer → load '%s' @%ldms",
                 cs.track_uri.c_str(), (long)cs.position_ms);

    // Derive GID from the base-62 track ID in the URI (spotify:track:<base62>)
    std::vector<uint8_t> gid;
    if (cs.track_uri.size() > 14 &&
        cs.track_uri.compare(0, 14, "spotify:track:") == 0) {
        gid = base62_to_gid(cs.track_uri.c_str() + 14,
                             cs.track_uri.size() - 14);
    }

    int pos = (int)cs.position_ms;
    if (callbacks_.on_play) callbacks_.on_play(cs.track_uri, pos);

    // If the new track is already in our loaded full context, just advance the
    // index — no Mercury context-resolve needed.  This avoids saturating the AP
    // connection with a full playlist response on every auto-advance.
    bool same_context   = (context_uri_ == cs.context_uri) && !context_uri_.empty();
    bool need_ctx_fetch = true;
    context_uri_       = cs.context_uri;
    current_track_uri_ = cs.track_uri;

    if (same_context) {
        for (size_t k = 0; k < track_uris_.size(); ++k) {
            if (track_uris_[k] == cs.track_uri) {
                playing_track_idx_ = (uint32_t)k;
                abs_track_idx_ = (cs.context_index_track != UINT32_MAX)
                                 ? cs.context_index_track : (uint32_t)k;
                if (gid.size() == 16 && k < track_gids_.size())
                    track_gids_[k] = gid;
                WHBLogPrintf("spirc: cluster advance in-ctx idx=%zu abs=%u",
                             k, abs_track_idx_);
                need_ctx_fetch = false;
                break;
            }
        }
    }

    if (need_ctx_fetch) {
        // Build cluster window as initial track list; Mercury will expand it.
        std::vector<std::string> all_uris, all_uids;
        for (size_t k = 0; k < cs.prev_uris.size(); ++k) {
            all_uris.push_back(cs.prev_uris[k]);
            all_uids.push_back(k < cs.prev_uids.size() ? cs.prev_uids[k] : "");
        }
        size_t cur_idx = all_uris.size();
        all_uris.push_back(cs.track_uri);
        all_uids.push_back(cs.track_uid);
        for (size_t k = 0; k < cs.next_uris.size(); ++k) {
            all_uris.push_back(cs.next_uris[k]);
            all_uids.push_back(k < cs.next_uids.size() ? cs.next_uids[k] : "");
        }
        track_uris_        = all_uris;
        track_uids_        = all_uids;
        track_gids_.assign(all_uris.size(), {});
        if (gid.size() == 16) track_gids_[cur_idx] = gid;
        playing_track_idx_ = (uint32_t)cur_idx;
        abs_track_idx_ = (cs.context_index_track != UINT32_MAX)
                         ? cs.context_index_track : (uint32_t)cur_idx;
        WHBLogPrintf("spirc: cluster list=%zu prev=%zu next=%zu cur=%zu abs=%u",
                     all_uris.size(), cs.prev_uris.size(), cs.next_uris.size(),
                     cur_idx, abs_track_idx_);
    }

    playing_           = cs.is_playing;
    pos_ms_            = pos;
    state_update_id_   = 0;
    duration_ms_       = 0;  // reset; fetch_track_metadata will populate it
    if (cs.is_playing && started_playing_at_ms_ == 0)
        started_playing_at_ms_ = (int64_t)time(nullptr) * 1000;

    if (gid.size() == 16) fetch_track_metadata(gid, pos);
    if (need_ctx_fetch) fetch_context_tracks(cs.context_uri, cs.track_uri);
    put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, cs.is_playing, pos, vol_pct_);
}

// ── Dealer request (player command) handler ───────────────────────────────────
// Called from the Dealer thread for type:"request" frames.
// Parses the command JSON and dispatches to the appropriate callbacks.

bool Spirc::handle_dealer_request(const std::string & /*message_ident*/,
                                   const std::string &cmd_json) {
    WHBLogPrintf("spirc: cmd-raw=%.250s", cmd_json.c_str());
    cJSON *root = cJSON_Parse(cmd_json.c_str());
    if (!root) {
        WHBLogPrint("spirc: cmd: JSON parse failed");
        return false;
    }

    // The Request JSON nests the command under a "command" key:
    //   {"message_id":1,"sent_by_device_id":"...","command":{"endpoint":"play",...}}
    // Fall back to root if the command object is missing (direct endpoint format).
    cJSON *cmd_obj = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsObject(cmd_obj)) cmd_obj = root;

    cJSON *ep_j = cJSON_GetObjectItem(cmd_obj, "endpoint");
    if (!cJSON_IsString(ep_j)) {
        cJSON_Delete(root);
        return false;
    }
    const char *ep = ep_j->valuestring;
    WHBLogPrintf("spirc: cmd endpoint='%s'", ep);

    // ── pause / resume ────────────────────────────────────────────────────────
    if (strcmp(ep, "pause") == 0) {
        playing_ = false;
        if (callbacks_.on_playing_changed) callbacks_.on_playing_changed(false, pos_ms_);
        put_connect_state_async(4, false, pos_ms_, vol_pct_);

    } else if (strcmp(ep, "resume") == 0) {
        playing_ = true;
        if (started_playing_at_ms_ == 0)
            started_playing_at_ms_ = (int64_t)time(nullptr) * 1000;
        if (callbacks_.on_playing_changed) callbacks_.on_playing_changed(true, pos_ms_);
        put_connect_state_async(4, true, pos_ms_, vol_pct_);

    // ── seek_to ───────────────────────────────────────────────────────────────
    } else if (strcmp(ep, "seek_to") == 0) {
        cJSON *val_j = cJSON_GetObjectItem(cmd_obj, "value");
        if (!val_j) val_j = cJSON_GetObjectItem(cmd_obj, "position");
        int pos = val_j ? (int)val_j->valuedouble : pos_ms_;
        WHBLogPrintf("spirc: seek_to %d ms", pos);
        pos_ms_ = pos;
        if (callbacks_.on_seek) callbacks_.on_seek(pos);
        put_connect_state_async(4, playing_, pos, vol_pct_);

    // ── skip_next / skip_prev ─────────────────────────────────────────────────
    } else if (strcmp(ep, "skip_next") == 0) {
        skip(true);

    } else if (strcmp(ep, "skip_prev") == 0) {
        skip(false);

    // ── transfer ──────────────────────────────────────────────────────────────
    // Sent when another device hands playback to us (e.g. "Play on Wii U").
    // data = base64(TransferState protobuf)
    } else if (strcmp(ep, "transfer") == 0) {
        cJSON *data_j = cJSON_GetObjectItem(cmd_obj, "data");
        if (!cJSON_IsString(data_j)) {
            WHBLogPrint("spirc: transfer: missing data");
            cJSON_Delete(root);
            return false;
        }
        auto proto = b64_decode(data_j->valuestring, strlen(data_j->valuestring));

        struct { std::string uri; std::vector<uint8_t> gid;
                 int32_t pos = 0; bool paused = false; std::string ctx; } ts;

        PRd rd; rd.p = proto.data(); rd.end = proto.data() + proto.size(); rd.ok = true;
        uint32_t f; uint8_t w;
        while (rd.next(f, w)) {
            if (f == 2 && w == 2) {                     // Playback
                PRd pb; rd.enter(pb);
                while (pb.next(f, w)) {
                    if      (f == 2 && w == 0) { uint64_t v; pb.vi(v); ts.pos = (int32_t)v; }
                    else if (f == 4 && w == 0) { uint64_t v; pb.vi(v); ts.paused = (v != 0); }
                    else if (f == 5 && w == 2) {         // ContextTrack
                        PRd ct; pb.enter(ct);
                        while (ct.next(f, w)) {
                            if      (f == 1 && w == 2) ct.read_str(ts.uri);
                            else if (f == 3 && w == 2) ct.read_bytes(ts.gid);
                            else                        ct.skip(w);
                        }
                    } else pb.skip(w);
                }
            } else if (f == 3 && w == 2) {              // Session
                PRd sess; rd.enter(sess);
                while (sess.next(f, w)) {
                    if (f == 2 && w == 2) {              // Context
                        PRd ctx; sess.enter(ctx);
                        while (ctx.next(f, w)) {
                            if (f == 1 && w == 2) ctx.read_str(ts.ctx);
                            else                   ctx.skip(w);
                        }
                    } else sess.skip(w);
                }
            } else rd.skip(w);
        }

        if (ts.uri.empty() && ts.gid.size() == 16)
            ts.uri = "spotify:track:" + gid_to_base62(ts.gid);
        if (ts.uri.empty()) {
            WHBLogPrint("spirc: transfer: no track URI");
            cJSON_Delete(root);
            return false;
        }
        WHBLogPrintf("spirc: transfer track='%.40s' pos=%d paused=%d ctx='%.30s'",
                     ts.uri.c_str(), ts.pos, (int)ts.paused, ts.ctx.c_str());

        context_uri_       = ts.ctx;
        current_track_uri_ = ts.uri;
        track_uris_        = {ts.uri};
        track_uids_        = {""};
        track_gids_        = {ts.gid};
        playing_track_idx_ = 0;
        abs_track_idx_     = 0;
        playing_           = !ts.paused;
        pos_ms_            = ts.pos;
        duration_ms_       = 0;
        state_update_id_   = 0;
        if (playing_ && started_playing_at_ms_ == 0)
            started_playing_at_ms_ = (int64_t)time(nullptr) * 1000;

        if (callbacks_.on_play) callbacks_.on_play(ts.uri, ts.pos);
        if (ts.gid.size() == 16) fetch_track_metadata(ts.gid, ts.pos);
        fetch_context_tracks(ts.ctx, ts.uri);
        put_connect_state_async(4, playing_, pos_ms_, vol_pct_);

    // ── play ──────────────────────────────────────────────────────────────────
    // Sent when the user picks a specific song/context in the app.
    } else if (strcmp(ep, "play") == 0) {
        cJSON *ctx_j  = cJSON_GetObjectItem(cmd_obj, "context");
        cJSON *opts_j = cJSON_GetObjectItem(cmd_obj, "options");

        std::string ctx_uri;
        if (cJSON_IsObject(ctx_j)) {
            cJSON *cu = cJSON_GetObjectItem(ctx_j, "uri");
            if (cJSON_IsString(cu)) ctx_uri = cu->valuestring;
        }

        int  seek_to      = 0;
        int  skip_idx     = 0;
        std::string skip_uri;
        // Tri-state: -1=not present in command, 0=false, 1=true
        int  override_shuffle       = -1;
        int  override_repeat        = -1;
        int  override_repeat_track  = -1;
        if (cJSON_IsObject(opts_j)) {
            cJSON *sk = cJSON_GetObjectItem(opts_j, "seek_to");
            if (sk) seek_to = (int)sk->valuedouble;
            cJSON *st = cJSON_GetObjectItem(opts_j, "skip_to");
            if (cJSON_IsObject(st)) {
                cJSON *tu = cJSON_GetObjectItem(st, "track_uri");
                if (cJSON_IsString(tu)) skip_uri = tu->valuestring;
                cJSON *ti = cJSON_GetObjectItem(st, "track_index");
                if (ti) skip_idx = (int)ti->valuedouble;
            }
            // player_options_override is a base64-encoded binary proto
            // (ContextPlayerOptionOverrides: shuffling_context=1, repeating_context=2)
            cJSON *po = cJSON_GetObjectItem(opts_j, "player_options_override");
            if (cJSON_IsString(po)) {
                auto raw = b64_decode(po->valuestring, strlen(po->valuestring));
                PRd rd{raw.data(), raw.data() + raw.size(), true};
                uint32_t f; uint8_t w;
                while (rd.next(f, w)) {
                    if (w == 0) {
                        uint64_t v; rd.vi(v);
                        if      (f == 1) override_shuffle      = v ? 1 : 0;
                        else if (f == 2) override_repeat       = v ? 1 : 0;
                        else if (f == 3) override_repeat_track = v ? 1 : 0;
                    } else rd.skip(w);
                }
                WHBLogPrintf("spirc: play player_options_override sh=%d rpt=%d rpt1=%d",
                             override_shuffle, override_repeat, override_repeat_track);
            }
        }

        // Collect tracks from context pages[0].tracks
        std::vector<std::string> uris;
        std::vector<std::string> uids;
        if (cJSON_IsObject(ctx_j)) {
            cJSON *pages = cJSON_GetObjectItem(ctx_j, "pages");
            if (cJSON_IsArray(pages) && cJSON_GetArraySize(pages) > 0) {
                cJSON *p0 = cJSON_GetArrayItem(pages, 0);
                cJSON *tracks = p0 ? cJSON_GetObjectItem(p0, "tracks") : nullptr;
                if (cJSON_IsArray(tracks)) {
                    int n = cJSON_GetArraySize(tracks);
                    for (int i = 0; i < n; i++) {
                        cJSON *t  = cJSON_GetArrayItem(tracks, i);
                        cJSON *u  = t ? cJSON_GetObjectItem(t, "uri") : nullptr;
                        cJSON *id = t ? cJSON_GetObjectItem(t, "uid") : nullptr;
                        if (cJSON_IsString(u)) uris.push_back(u->valuestring);
                        uids.push_back(cJSON_IsString(id) ? id->valuestring : "");
                    }
                }
            }
        }

        // Determine starting track
        std::string target_uri = skip_uri;
        int target_idx = 0;
        if (target_uri.empty() && !uris.empty()) {
            target_idx  = std::min(skip_idx, (int)uris.size() - 1);
            target_uri  = uris[target_idx];
        } else if (!target_uri.empty() && !uris.empty()) {
            for (int i = 0; i < (int)uris.size(); i++) {
                if (uris[i] == target_uri) { target_idx = i; break; }
            }
        }

        if (target_uri.empty()) {
            WHBLogPrint("spirc: play: no target URI");
            cJSON_Delete(root);
            return false;
        }

        std::vector<uint8_t> gid;
        if (target_uri.size() > 14 && target_uri.compare(0, 14, "spotify:track:") == 0)
            gid = base62_to_gid(target_uri.c_str() + 14, target_uri.size() - 14);

        WHBLogPrintf("spirc: play track='%.40s' idx=%d pos=%d ctx='%.30s'",
                     target_uri.c_str(), target_idx, seek_to, ctx_uri.c_str());

        context_uri_       = ctx_uri;
        current_track_uri_ = target_uri;
        if (!uris.empty()) {
            track_uris_ = uris;
            track_uids_ = uids;
            track_uids_.resize(uris.size());  // pad to same length if uids was shorter
            track_gids_.assign(uris.size(), {});
            if (gid.size() == 16) track_gids_[target_idx] = gid;
        } else {
            track_uris_ = {target_uri};
            track_uids_ = {""};
            track_gids_ = {gid};
        }
        playing_track_idx_ = target_idx;
        abs_track_idx_     = (uint32_t)target_idx;
        playing_           = true;
        pos_ms_            = seek_to;
        duration_ms_       = 0;
        state_update_id_   = 0;
        if (started_playing_at_ms_ == 0)
            started_playing_at_ms_ = (int64_t)time(nullptr) * 1000;

        if (override_shuffle      >= 0) shuffle_      = (override_shuffle      == 1);
        if (override_repeat       >= 0) repeat_       = (override_repeat       == 1);
        if (override_repeat_track >= 0) repeat_track_ = (override_repeat_track == 1);

        if (callbacks_.on_play) callbacks_.on_play(target_uri, seek_to);
        if (gid.size() == 16) fetch_track_metadata(gid, seek_to);
        fetch_context_tracks(ctx_uri, target_uri);
        put_connect_state_async(4, true, seek_to, vol_pct_);

    // ── set_options (shuffle / repeat) ───────────────────────────────────────
    } else if (strcmp(ep, "set_options") == 0) {
        cJSON *sh = cJSON_GetObjectItem(cmd_obj, "shuffling_context");
        cJSON *rc = cJSON_GetObjectItem(cmd_obj, "repeating_context");
        cJSON *rt = cJSON_GetObjectItem(cmd_obj, "repeating_track");
        if (cJSON_IsBool(sh)) shuffle_      = cJSON_IsTrue(sh);
        if (cJSON_IsBool(rc)) repeat_       = cJSON_IsTrue(rc);
        if (cJSON_IsBool(rt)) repeat_track_ = cJSON_IsTrue(rt);
        WHBLogPrintf("spirc: set_options shuffle=%d repeat_ctx=%d repeat_track=%d",
                     (int)shuffle_, (int)repeat_, (int)repeat_track_);
        if (callbacks_.on_options_changed) callbacks_.on_options_changed(shuffle_, repeat_mode());
        put_connect_state_async(4, playing_, pos_ms_, vol_pct_);

    // ── set_shuffling_context / set_repeating_context / set_repeating_track ──
    } else if (strcmp(ep, "set_shuffling_context") == 0) {
        cJSON *v = cJSON_GetObjectItem(cmd_obj, "value");
        if (cJSON_IsBool(v)) shuffle_ = cJSON_IsTrue(v);
        WHBLogPrintf("spirc: set_shuffling_context=%d", (int)shuffle_);
        if (callbacks_.on_options_changed) callbacks_.on_options_changed(shuffle_, repeat_mode());
        put_connect_state_async(4, playing_, pos_ms_, vol_pct_);

    } else if (strcmp(ep, "set_repeating_context") == 0) {
        cJSON *v = cJSON_GetObjectItem(cmd_obj, "value");
        if (cJSON_IsBool(v)) repeat_ = cJSON_IsTrue(v);
        WHBLogPrintf("spirc: set_repeating_context=%d", (int)repeat_);
        if (callbacks_.on_options_changed) callbacks_.on_options_changed(shuffle_, repeat_mode());
        put_connect_state_async(4, playing_, pos_ms_, vol_pct_);

    } else if (strcmp(ep, "set_repeating_track") == 0) {
        cJSON *v = cJSON_GetObjectItem(cmd_obj, "value");
        if (cJSON_IsBool(v)) repeat_track_ = cJSON_IsTrue(v);
        WHBLogPrintf("spirc: set_repeating_track=%d", (int)repeat_track_);
        if (callbacks_.on_options_changed) callbacks_.on_options_changed(shuffle_, repeat_mode());
        put_connect_state_async(4, playing_, pos_ms_, vol_pct_);

    // ── set_volume ────────────────────────────────────────────────────────────
    } else if (strcmp(ep, "set_volume") == 0) {
        cJSON *vol_j = cJSON_GetObjectItem(cmd_obj, "volume");
        WHBLogPrintf("spirc: set_volume vol_found=%d", vol_j!=nullptr);
        if (cJSON_IsNumber(vol_j)) {
            int raw = (int)vol_j->valuedouble;  // 0–65535
            int pct = (int)((uint64_t)raw * 100 / 65535);
            vol_pct_ = pct;
            if (callbacks_.on_volume) callbacks_.on_volume(pct);
            put_connect_state_async(4, playing_, pos_ms_, pct);
        }

    } else {
        WHBLogPrintf("spirc: cmd unknown endpoint '%s'", ep);
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> Spirc::build_frame(uint32_t msg_type, bool playing,
                                         int pos_ms, int vol_pct) {
    // Frame field numbers from spirc.proto (hex values):
    //   version=1  ident=2  protocol_version=3  seq_nr=0x4=4  typ=0x5=5
    //   device_state=0x7=7  state=0xc=12  volume=0xe=14
    // State field numbers from spirc.proto: position_ms=0x4=4  status=0x5=5
    // PlayStatus: kPlayStatusPlay=0x1=1  kPlayStatusPause=0x2=2
    std::vector<uint8_t> state;
    if (!context_uri_.empty()) pb_str(state, 2, context_uri_);
    pb_u32(state, 4, (uint32_t)(pos_ms > 0 ? pos_ms : 0));
    pb_u32(state, 5, playing ? 1u : 2u);
    pb_u32(state, 26, playing_track_idx_);
    // Only echo the currently playing track — the full list can be 5-6 KB and
    // Mercury drops oversized messages silently. The controller already has the list.
    {
        size_t i = playing_track_idx_;
        std::vector<uint8_t> tr;
        if (i < track_gids_.size() && !track_gids_[i].empty())
            pb_bytes(tr, 1, track_gids_[i].data(), track_gids_[i].size());
        if (i < track_uris_.size() && !track_uris_[i].empty())
            pb_str(tr, 2, track_uris_[i]);
        if (!tr.empty()) pb_msg(state, 27, tr);
    }

    std::vector<uint8_t> frame;
    pb_u32(frame,  1, 1);                          // version
    pb_str(frame,  2, device_id_);                 // ident
    pb_str(frame,  3, "2.0.0");                    // protocol_version
    pb_u32(frame,  4, frame_seq_.fetch_add(1));    // seq_nr
    pb_u32(frame,  5, msg_type);                   // typ
    pb_msg(frame,  7, build_device_state(
        msg_type == MsgType::Hello || msg_type == MsgType::Notify, vol_pct));
    pb_msg(frame, 12, state);                      // state
    pb_u32(frame, 14, (uint32_t)(vol_pct * 655)); // volume
    if (state_update_id_ != 0)
        pb_i64(frame, 17, state_update_id_);       // state_update_id = 0x11 (echo from Load)
    return frame;
}

// ── Public interface ──────────────────────────────────────────────────────────

void Spirc::send_hello() {
    std::string uri = "hm://remote/3/user/" + username_ + "/";
    mercury_send(uri, build_frame(MsgType::Hello, false, 0, vol_pct_));
}

void Spirc::send_notify(bool playing, int pos_ms, int vol_pct) {
    std::string uri = "hm://remote/3/user/" + username_ + "/";
    mercury_send(uri, build_frame(MsgType::Notify, playing, pos_ms, vol_pct));
}

void Spirc::notify(bool playing, int pos_ms, int vol_pct) {
    bool playing_changed = (playing != playing_);
    bool vol_changed     = (vol_pct  != vol_pct_);
    playing_ = playing; pos_ms_ = pos_ms; vol_pct_ = vol_pct;
    if (playing_changed && callbacks_.on_playing_changed)
        callbacks_.on_playing_changed(playing, pos_ms);
    // Legacy Mercury Notify only on meaningful changes — Spotify rejects SEND to the
    // broadcast channel with 400, so flooding every 1 s for position-only updates is wasteful.
    if ((playing_changed || vol_changed) && started_.load())
        send_notify(playing, pos_ms, vol_pct);
    // Push connect-state on meaningful changes, or every 15 s while playing so the
    // host app's seek bar has a fresh position_as_of_timestamp to interpolate from.
    int64_t now_ms = now_ms_wiiu();
    if (playing_changed)
        put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, playing, pos_ms, vol_pct);
    else if (vol_changed)
        put_connect_state_async(5 /* VOLUME_CHANGED */, playing, pos_ms, vol_pct);
    else if (playing && helo_done_.load() &&
             now_ms - last_state_push_ms_ >= 1000)
        put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, playing, pos_ms, vol_pct);
}

void Spirc::skip(bool next_track) {
    size_t n = track_gids_.size();
    if (n == 0) return;
    if (next_track) {
        if (shuffle_ && n > 1) {
            size_t new_idx;
            do { new_idx = (size_t)rand() % n; } while (new_idx == playing_track_idx_);
            playing_track_idx_ = new_idx;
            // For shuffle the absolute index is arbitrary; advance by 1 as a placeholder
            abs_track_idx_++;
        } else {
            playing_track_idx_ = (playing_track_idx_ + 1) % n;
            abs_track_idx_++;
        }
    } else {
        playing_track_idx_ = (playing_track_idx_ == 0) ? n - 1 : playing_track_idx_ - 1;
        if (abs_track_idx_ > 0) abs_track_idx_--;
    }

    const std::string &uri = playing_track_idx_ < track_uris_.size()
                           ? track_uris_[playing_track_idx_] : std::string{};

    // GID may be absent for queue entries populated from a play command's inline
    // track list.  Derive it from the base-62 URI so metadata fetch always fires.
    std::vector<uint8_t> gid = track_gids_[playing_track_idx_];
    if (gid.size() != 16 && uri.size() > 14 && uri.compare(0, 14, "spotify:track:") == 0) {
        gid = base62_to_gid(uri.c_str() + 14, uri.size() - 14);
        track_gids_[playing_track_idx_] = gid;
    }

    current_track_uri_ = uri;
    WHBLogPrintf("spirc: local %s → idx=%u uri=%.40s", next_track ? "Next" : "Prev",
                 playing_track_idx_, uri.c_str());
    if (!uri.empty() && callbacks_.on_play) callbacks_.on_play(uri, 0);
    if (gid.size() == 16) fetch_track_metadata(gid, 0);
    playing_ = true; pos_ms_ = 0;
    if (started_playing_at_ms_ == 0)
        started_playing_at_ms_ = (int64_t)time(nullptr) * 1000;
    if (callbacks_.on_playing_changed) callbacks_.on_playing_changed(true, 0);
    if (started_.load()) send_notify(true, 0, vol_pct_);
    put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, true, 0, vol_pct_);
}

void Spirc::seek_to(int pos_ms) {
    pos_ms_ = pos_ms;
    if (callbacks_.on_seek) callbacks_.on_seek(pos_ms);
    if (started_.load()) send_notify(playing_, pos_ms, vol_pct_);
    put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, playing_, pos_ms, vol_pct_);
}

void Spirc::toggle_shuffle() {
    shuffle_ = !shuffle_;
    WHBLogPrintf("spirc: shuffle=%d", (int)shuffle_);
    put_connect_state_async(4, playing_, pos_ms_, vol_pct_);
}

void Spirc::toggle_repeat() {
    // Cycle: off → repeat-context → repeat-track → off
    if (!repeat_ && !repeat_track_) {
        repeat_ = true;
    } else if (repeat_ && !repeat_track_) {
        repeat_ = false; repeat_track_ = true;
    } else {
        repeat_ = false; repeat_track_ = false;
    }
    WHBLogPrintf("spirc: repeat ctx=%d track=%d", (int)repeat_, (int)repeat_track_);
    put_connect_state_async(4, playing_, pos_ms_, vol_pct_);
}

void Spirc::notify_track_end(int vol_pct) {
    // Signal natural completion: is_playing=false, is_paused=false.
    // Resetting started_playing_at_ms_ causes build_player_state to emit
    // is_paused=false (it's conditioned on that field being > 0), which tells
    // Spotify the device became inactive rather than paused, triggering auto-advance.
    playing_              = false;
    pos_ms_               = (int)duration_ms_;
    vol_pct_              = vol_pct;
    started_playing_at_ms_ = 0;
    if (started_.load()) send_notify(false, pos_ms_, vol_pct);
    put_connect_state_async(4, false, pos_ms_, vol_pct);
}

void Spirc::replay_current() {
    size_t i = playing_track_idx_;
    if (i >= track_uris_.size() || track_uris_[i].empty()) return;
    const std::string       &uri = track_uris_[i];
    const std::vector<uint8_t> &gid = (i < track_gids_.size()) ? track_gids_[i]
                                                                 : track_gids_[0];
    current_track_uri_ = uri;
    WHBLogPrintf("spirc: replay idx=%zu uri=%.40s", i, uri.c_str());
    playing_ = true; pos_ms_ = 0;
    if (started_playing_at_ms_ == 0)
        started_playing_at_ms_ = (int64_t)time(nullptr) * 1000;
    if (callbacks_.on_play) callbacks_.on_play(uri, 0);
    if (gid.size() == 16) fetch_track_metadata(gid, 0);
    if (callbacks_.on_playing_changed) callbacks_.on_playing_changed(true, 0);
    if (started_.load()) send_notify(true, 0, vol_pct_);
    put_connect_state_async(4, true, 0, vol_pct_);
}

// ── on_packet ─────────────────────────────────────────────────────────────────

void Spirc::on_packet(uint8_t cmd, std::vector<uint8_t> payload) {
    if      (cmd == Cmd::MercuryReq)   handle_mercury_response(payload);
    else if (cmd == Cmd::MercuryEvent) handle_mercury_event(payload);
    // MercuryAck (0xB6), MercurySub (0xB3): nothing to do
}

// ── Mercury inbound ───────────────────────────────────────────────────────────

std::vector<std::vector<uint8_t>> Spirc::parse_parts(const std::vector<uint8_t> &payload) {
    // Mercury wire format: [2B seq_len] [seq_len bytes] [1B flags] [2B count] [parts...]
    std::vector<std::vector<uint8_t>> parts;
    if (payload.size() < 5) return parts;
    uint16_t seq_len = ((uint16_t)payload[0] << 8) | payload[1];
    size_t off = 2 + seq_len;            // skip seq_len + seq bytes
    if (off + 3 > payload.size()) return parts;
    // uint8_t flags = payload[off];     // not used currently
    off++;                               // skip flags
    uint16_t count = ((uint16_t)payload[off] << 8) | payload[off + 1];
    off += 2;
    for (uint16_t i = 0; i < count && off + 2 <= payload.size(); ++i) {
        uint16_t plen = ((uint16_t)payload[off] << 8) | payload[off + 1];
        off += 2;
        if (off + plen > payload.size()) break;
        parts.emplace_back(payload.begin() + off, payload.begin() + off + plen);
        off += plen;
    }
    return parts;
}

void Spirc::handle_mercury_response(const std::vector<uint8_t> &payload) {
    // Wire format: [2B seq_len] [seq_len bytes seq] ...
    // seq_len is always 4 for our outgoing requests; server echoes same format.
    if (payload.size() < 9) return;  // 2 + 4 + 1(flags) + 2(count) minimum
    uint16_t seq_len = ((uint16_t)payload[0] << 8) | payload[1];
    if (seq_len < 4 || payload.size() < (size_t)(2 + seq_len + 3)) return;
    uint32_t seq = ((uint32_t)payload[2] << 24) | ((uint32_t)payload[3] << 16) |
                   ((uint32_t)payload[4] <<  8) |  (uint32_t)payload[5];

    auto parts = parse_parts(payload);

    std::function<void(bool, std::vector<std::vector<uint8_t>>)> cb;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_.find(seq);
        if (it == pending_.end()) return;  // SEND/SUB ack — no callback, discard silently
        cb = std::move(it->second.cb);
        pending_.erase(it);
    }

    // Log responses that we registered callbacks for (GET requests).
    if (!parts.empty()) {
        PRd hdr{parts[0].data(), parts[0].data() + parts[0].size()};
        std::string uri; uint64_t status = 0; uint32_t hf; uint8_t hw;
        while (hdr.next(hf, hw)) {
            if      (hf == 1 && hw == 2) hdr.read_str(uri);
            else if (hf == 4 && hw == 0) hdr.vi(status);
            else hdr.skip(hw);
        }
        if (parts.size() < 2 || status >= 400)
            WHBLogPrintf("spirc: mercury_resp seq=%u status=%llu parts=%zu uri='%s'",
                         seq, (unsigned long long)status, parts.size(), uri.c_str());
    }

    if (cb) cb(true, std::move(parts));
}

void Spirc::handle_mercury_event(const std::vector<uint8_t> &payload) {
    auto parts = parse_parts(payload);
    std::string uri;
    if (!parts.empty()) {
        PRd hdr{parts[0].data(), parts[0].data() + parts[0].size()};
        uint64_t status = 0; uint32_t hf; uint8_t hw;
        while (hdr.next(hf, hw)) {
            if      (hf == 1 && hw == 2) hdr.read_str(uri);
            else if (hf == 4 && hw == 0) hdr.vi(status);
            else hdr.skip(hw);
        }
    }

    if (parts.size() < 2 || parts[1].empty()) return;

    // SetVolumeCommand arrives as a binary proto on the connect/volume subscription.
    // connect.proto: SetVolumeCommand { int32 volume = 1; ... }
    if (uri == "hm://connect-state/v1/connect/volume") {
        PRd vrd{parts[1].data(), parts[1].data() + parts[1].size()};
        uint32_t vf; uint8_t vw;
        while (vrd.next(vf, vw)) {
            if (vf == 1 && vw == 0) {
                uint64_t v; vrd.vi(v);
                int raw = (int)v;  // 0–65535
                int pct = (int)((uint64_t)raw * 100 / 65535);
                WHBLogPrintf("spirc: SetVolumeCommand vol=%d (%d%%)", raw, pct);
                vol_pct_ = pct;
                if (callbacks_.on_volume) callbacks_.on_volume(pct);
                put_connect_state_async(5 /* VOLUME_CHANGED */, playing_, pos_ms_, pct);
            } else vrd.skip(vw);
        }
        return;
    }

    // All other Mercury events: old Spirc frame format.
    handle_frame(parts[1]);
}

// ── Frame dispatch ────────────────────────────────────────────────────────────

void Spirc::handle_frame(const std::vector<uint8_t> &bytes) {
    PRd rd{bytes.data(), bytes.data() + bytes.size()};

    uint32_t    msg_type  = 0;
    std::string ident;
    uint32_t    volume    = 0;  // 0-65535 (field 9)
    uint32_t    position  = 0;  // field 8 (seek/start position)
    int64_t     state_update_id = 0;  // field 15 — echoed back in Notify

    // State (field 7)
    uint32_t state_pos_ms     = 0;
    uint32_t state_status     = 0; (void)state_status;
    uint32_t playing_track_idx = 0;
    std::vector<std::vector<uint8_t>> track_gids;
    std::vector<std::string>          track_uris;
    std::string context_uri;

    // Frame proto field numbers (from spirc.proto, hex values):
    //   ident=0x2=2  seq_nr=0x4=4  typ=0x5=5  device_state=0x7=7  state=0xc=12
    //   position=0xd=13  volume=0xe=14  state_update_id=0xf=15
    uint32_t f; uint8_t w;
    while (rd.next(f, w)) {
        switch (f) {
        case 2:  rd.read_str(ident); break;
        case 5:  { uint64_t v; rd.vi(v); msg_type = (uint32_t)v; break; }
        case 17: { uint64_t v; rd.vi(v); state_update_id = (int64_t)v; break; }
        case 12: {
            // State field numbers from spirc.proto (0x-prefixed = decimal value):
            //   context_uri=0x2=2  index=0x3=3  position_ms=0x4=4  status=0x5=5
            //   playing_track_index=0x1a=26  track=0x1b=27
            PRd st; rd.enter(st);
            uint32_t sf; uint8_t sw;
            while (st.next(sf, sw)) {
                switch (sf) {
                case 2:  st.read_str(context_uri); break;
                case 4:  { uint64_t v; st.vi(v); state_pos_ms = (uint32_t)v; break; }
                case 5:  { uint64_t v; st.vi(v); state_status = (uint32_t)v; break; }
                case 26: { uint64_t v; st.vi(v); playing_track_idx = (uint32_t)v; break; }
                case 27: {
                    // TrackRef: gid=0x1=1  uri=0x2=2
                    PRd tr; st.enter(tr);
                    std::vector<uint8_t> gid; std::string uri;
                    uint32_t tf; uint8_t tw;
                    while (tr.next(tf, tw)) {
                        if      (tf == 1) tr.read_bytes(gid);
                        else if (tf == 2) tr.read_str(uri);
                        else              tr.skip(tw);
                    }
                    track_gids.push_back(std::move(gid));
                    track_uris.push_back(std::move(uri));
                    break;
                }
                default: st.skip(sw); break;
                }
            }
            break;
        }
        case 13: { uint64_t v; rd.vi(v); position = (uint32_t)v; break; }
        case 14: { uint64_t v; rd.vi(v); volume   = (uint32_t)v; break; }
        default: rd.skip(w); break;
        }
    }

    // Ignore our own echoed Frames
    if (ident == device_id_) return;

    WHBLogPrintf("spirc: frame type=%u from='%s'", msg_type, ident.c_str());

    switch (msg_type) {
    case MsgType::Hello:
    case MsgType::Notify:
        // Another device announced its state — reply with ours
        if (started_.load()) send_notify(playing_, pos_ms_, vol_pct_);
        break;

    case MsgType::Load:
    case MsgType::Play: {
        size_t idx = (playing_track_idx < track_uris.size()) ? playing_track_idx : 0;

        std::string track_uri;
        std::vector<uint8_t> track_gid;

        if (idx < track_uris.size()) track_uri = track_uris[idx];
        if (idx < track_gids.size()) track_gid = track_gids[idx];

        // Fallback: derive GID from spotify:track: URI
        if (track_gid.empty() && track_uri.size() > 14 &&
            track_uri.compare(0, 14, "spotify:track:") == 0) {
            track_gid = base62_to_gid(track_uri.c_str() + 14,
                                      track_uri.size() - 14);
        }

        // Fallback: derive URI from GID (Mercury Load sometimes omits it)
        if (track_uri.empty() && track_gid.size() == 16)
            track_uri = "spotify:track:" + gid_to_base62(track_gid);

        // Fallback URI: context itself
        if (track_uri.empty()) track_uri = context_uri;

        int pos = state_pos_ms > 0 ? (int)state_pos_ms
                : position    > 0 ? (int)position : 0;

        WHBLogPrintf("spirc: Load tracks=%zu idx=%zu uri='%s' gid=%zu pos=%d",
                     track_uris.size(), idx, track_uri.c_str(),
                     track_gid.size(), pos);

        if (!track_uri.empty() && callbacks_.on_play)
            callbacks_.on_play(track_uri, pos);

        if (track_gid.size() == 16)
            fetch_track_metadata(track_gid, pos);

        playing_ = true; pos_ms_ = pos;
        context_uri_        = context_uri;
        playing_track_idx_  = (uint32_t)idx;
        track_gids_         = track_gids;
        track_uris_         = track_uris;
        // Backfill URI for current track if Mercury Load omitted it.
        {
            size_t ti = playing_track_idx_;
            if (ti < track_uris_.size() && track_uris_[ti].empty() &&
                ti < track_gids_.size() && track_gids_[ti].size() == 16)
                track_uris_[ti] = "spotify:track:" + gid_to_base62(track_gids_[ti]);
        }
        state_update_id_    = state_update_id;
        duration_ms_        = 0;  // reset; fetch_track_metadata will populate it
        if (started_playing_at_ms_ == 0)
            started_playing_at_ms_ = (int64_t)time(nullptr) * 1000;
        if (callbacks_.on_playing_changed) callbacks_.on_playing_changed(true, pos);
        if (started_.load()) send_notify(true, pos, vol_pct_);
        put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, true, pos, vol_pct_);
        break;
    }

    case MsgType::Pause:
        playing_ = false; pos_ms_ = (int)(state_pos_ms > 0 ? state_pos_ms : pos_ms_);
        if (callbacks_.on_playing_changed) callbacks_.on_playing_changed(false, pos_ms_);
        if (started_.load()) send_notify(false, pos_ms_, vol_pct_);
        put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, false, pos_ms_, vol_pct_);
        break;

    case MsgType::PlayPause:
        playing_ = !playing_;
        if (callbacks_.on_playing_changed) callbacks_.on_playing_changed(playing_, pos_ms_);
        if (started_.load()) send_notify(playing_, pos_ms_, vol_pct_);
        put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, playing_, pos_ms_, vol_pct_);
        break;

    case MsgType::Seek: {
        int pos = state_pos_ms > 0 ? (int)state_pos_ms
                : position     > 0 ? (int)position : 0;
        pos_ms_ = pos;
        if (callbacks_.on_seek) callbacks_.on_seek(pos);
        if (started_.load()) send_notify(playing_, pos_ms_, vol_pct_);
        put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, playing_, pos_ms_, vol_pct_);
        break;
    }

    case MsgType::Next:
    case MsgType::Prev: {
        size_t n = track_gids_.size();
        if (n == 0) break;
        if (msg_type == MsgType::Next)
            playing_track_idx_ = (playing_track_idx_ + 1) % n;
        else
            playing_track_idx_ = (playing_track_idx_ == 0) ? n - 1 : playing_track_idx_ - 1;

        const auto &gid = track_gids_[playing_track_idx_];
        const auto &uri = playing_track_idx_ < track_uris_.size()
                        ? track_uris_[playing_track_idx_] : std::string{};
        WHBLogPrintf("spirc: %s → idx=%u uri='%s'",
                     msg_type == MsgType::Next ? "Next" : "Prev",
                     playing_track_idx_, uri.c_str());
        if (!uri.empty() && callbacks_.on_play) callbacks_.on_play(uri, 0);
        if (gid.size() == 16) fetch_track_metadata(gid, 0);
        playing_ = true; pos_ms_ = 0;
        state_update_id_ = state_update_id;
        if (started_playing_at_ms_ == 0)
            started_playing_at_ms_ = (int64_t)time(nullptr) * 1000;
        if (callbacks_.on_playing_changed) callbacks_.on_playing_changed(true, 0);
        if (started_.load()) send_notify(true, 0, vol_pct_);
        put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, true, 0, vol_pct_);
        break;
    }

    case MsgType::Volume: {
        int pct = (int)((uint64_t)volume * 100 / 65535);
        vol_pct_ = pct;
        if (callbacks_.on_volume) callbacks_.on_volume(pct);
        if (started_.load()) send_notify(playing_, pos_ms_, pct);
        put_connect_state_async(5 /* VOLUME_CHANGED */, playing_, pos_ms_, pct);
        break;
    }

    case MsgType::Goodbye:
        break; // nothing to do when another device leaves

    default:
        WHBLogPrintf("spirc: unhandled frame type=%u", msg_type);
        break;
    }
}


// ── spclient token acquisition + CDN URL resolve (all HTTP, no AP Mercury) ───

static const char *KEYMASTER_CLIENT_ID = "65b708073fc0480ea92a077233ca87bd";

static std::vector<uint8_t> http_post_proto(
    const std::string &url, const std::vector<uint8_t> &body,
    std::initializer_list<const char *> extra_headers)
{
    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/x-protobuf");
    hdrs = curl_slist_append(hdrs, "Accept: application/x-protobuf");
    for (const char *h : extra_headers) hdrs = curl_slist_append(hdrs, h);
    std::vector<uint8_t> resp;
    CURL *curl = curl_easy_init();
    if (!curl) { curl_slist_free_all(hdrs); return {}; }
    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_POST,          1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char *p, size_t sz, size_t n, void *ud) -> size_t {
            auto *out = static_cast<std::vector<uint8_t> *>(ud);
            out->insert(out->end(), p, p + sz * n); return sz * n;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs); curl_easy_cleanup(curl);
    if (rc != CURLE_OK || code < 200 || code >= 300) {
        WHBLogPrintf("spirc: POST %s → %ld", url.c_str(), code); return {};
    }
    return resp;
}

// SHA-1 hash-cash for login5 challenges (same algorithm as librespot util::solve_hash_cash).
static std::vector<uint8_t> solve_hashcash(
    const std::vector<uint8_t> &ctx, const std::vector<uint8_t> &prefix, int32_t bits)
{
    uint8_t md[20];
    mbedtls_sha1_context s; mbedtls_sha1_init(&s);
    mbedtls_sha1_starts_ret(&s);
    mbedtls_sha1_update_ret(&s, ctx.data(), ctx.size());
    mbedtls_sha1_finish_ret(&s, md); mbedtls_sha1_free(&s);

    int64_t target = 0;
    for (int i = 12; i < 20; i++) target = (target << 8) | md[i];

    uint8_t suffix[16];
    for (int64_t ctr = 0; ctr < 10'000'000LL; ctr++) {
        int64_t v1 = target + ctr, v2 = ctr;
        for (int i = 7; i >= 0; i--) {
            suffix[i]   = (uint8_t)(v1 & 0xFF); v1 >>= 8;
            suffix[8+i] = (uint8_t)(v2 & 0xFF); v2 >>= 8;
        }
        uint8_t hash[20];
        mbedtls_sha1_init(&s); mbedtls_sha1_starts_ret(&s);
        mbedtls_sha1_update_ret(&s, prefix.data(), prefix.size());
        mbedtls_sha1_update_ret(&s, suffix, 16);
        mbedtls_sha1_finish_ret(&s, hash); mbedtls_sha1_free(&s);
        int64_t hval = 0;
        for (int i = 12; i < 20; i++) hval = (hval << 8) | hash[i];
        if ((hval == 0 ? 64 : __builtin_ctzll((uint64_t)hval)) >= bits)
            return {suffix, suffix + 16};
    }
    return {};
}

// Decode hex string → bytes (case-insensitive).
// Step 1: login5 — POST StoredCredential directly, no client-token needed.
// The reusable_auth_credentials from APWelcome (195B) work as StoredCredential.data.
static std::string get_access_token(
    const std::string &username,
    const std::vector<uint8_t> &auth_data,
    const std::string &device_id)
{
    std::vector<uint8_t> ci; pb_str(ci, 1, KEYMASTER_CLIENT_ID); pb_str(ci, 2, device_id);
    std::vector<uint8_t> sc; pb_str(sc, 1, username);
    pb_bytes(sc, 2, auth_data.data(), auth_data.size());

    std::vector<uint8_t> login_ctx, sol;
    for (int attempt = 0; attempt < 3; attempt++) {
        std::vector<uint8_t> req; pb_msg(req, 1, ci);
        if (!login_ctx.empty()) pb_bytes(req, 2, login_ctx.data(), login_ctx.size());
        if (!sol.empty()) {
            std::vector<uint8_t> dur; pb_u32(dur, 1, 0); pb_u32(dur, 2, 0);
            std::vector<uint8_t> hs; pb_bytes(hs, 1, sol.data(), sol.size()); pb_msg(hs, 2, dur);
            std::vector<uint8_t> cs; pb_msg(cs, 1, hs);
            std::vector<uint8_t> ss; pb_msg(ss, 1, cs);
            pb_msg(req, 3, ss);
        }
        pb_msg(req, 100, sc);  // stored_credential = field 100 (oneof)

        // No client-token header needed for StoredCredential login5.
        auto resp = http_post_proto("https://login5.spotify.com/v3/login", req, {});
        if (resp.empty()) return {};

        PRd rd{resp.data(), resp.data() + resp.size()};
        std::string atk; bool got_ok=false, got_err=false, got_ch=false;
        int32_t hc_len=0; std::vector<uint8_t> hc_pfx;
        uint32_t f; uint8_t w;
        while (rd.next(f, w)) {
            if (f == 1 && w == 2) {
                PRd ok; rd.enter(ok); uint32_t of; uint8_t ow;
                while (ok.next(of, ow)) {
                    if (of == 2 && ow == 2) ok.read_str(atk); else ok.skip(ow);
                }
                got_ok = true;
            } else if (f == 2 && w == 0) { uint64_t v; rd.vi(v); got_err=true;
                WHBLogPrintf("spirc: login5 error=%llu", (unsigned long long)v);
            } else if (f == 3 && w == 2) {
                PRd ch; rd.enter(ch); uint32_t cf; uint8_t cw;
                while (ch.next(cf, cw)) {
                    if (cf == 1 && cw == 2) {
                        PRd c; ch.enter(c); uint32_t ccf; uint8_t ccw;
                        while (c.next(ccf, ccw)) {
                            if (ccf == 1 && ccw == 2) {
                                PRd hc; c.enter(hc); uint32_t hf; uint8_t hw;
                                while (hc.next(hf, hw)) {
                                    if      (hf==1 && hw==0) { uint64_t v; hc.vi(v); hc_len=(int32_t)v; }
                                    else if (hf==2 && hw==2) hc.read_bytes(hc_pfx);
                                    else hc.skip(hw);
                                }
                                got_ch = true;
                            } else c.skip(ccw);
                        }
                    } else ch.skip(cw);
                }
            } else if (f == 5 && w == 2) { rd.read_bytes(login_ctx);
            } else rd.skip(w);
        }
        if (got_ok && !atk.empty()) {
            WHBLogPrintf("spirc: login5 access_token (%zu chars)", atk.size()); return atk;
        }
        if (got_err || !got_ch) return {};
        WHBLogPrintf("spirc: login5 hashcash bits=%d", hc_len);
        sol = solve_hashcash(login_ctx, hc_pfx, hc_len);
        if (sol.empty()) { WHBLogPrint("spirc: hashcash failed"); return {}; }
        WHBLogPrint("spirc: hashcash solved, retrying login5");
    }
    return {};
}

// Resolve the spclient hostname from apresolve.spotify.com (JSON response).
static std::string resolve_spclient_host() {
    std::string body;
    CURL *curl = curl_easy_init();
    if (!curl) return {};
    curl_easy_setopt(curl, CURLOPT_URL, "https://apresolve.spotify.com/?type=spclient");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char *p, size_t sz, size_t n, void *ud) -> size_t {
            ((std::string *)ud)->append(p, sz * n); return sz * n;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    // {"spclient":["host:port",...]} — extract first hostname, strip :port
    const char *key = "\"spclient\":[\"";
    auto pos = body.find(key);
    if (pos == std::string::npos) return {};
    pos += strlen(key);
    auto end = body.find('"', pos);
    std::string entry = body.substr(pos, end - pos);
    auto colon = entry.rfind(':');
    if (colon != std::string::npos) entry = entry.substr(0, colon);
    WHBLogPrintf("spirc: spclient host: %s", entry.c_str());
    return entry;
}

std::string Spirc::resolve_cdn_http(const std::vector<uint8_t> &file_id) {
    std::string atk;
    { std::lock_guard<std::mutex> lk(token_mu_); atk = access_token_; }

    if (atk.empty()) {
        atk = get_access_token(username_, ap_->reusable_creds(), device_id_);
        if (atk.empty()) { WHBLogPrint("spirc: login5 failed"); return {}; }
        std::lock_guard<std::mutex> lk(token_mu_);
        access_token_ = atk;
    }

    // Resolve actual spclient host via apresolve (spclientopen.spotify.com is defunct).
    std::string spclient = resolve_spclient_host();
    if (spclient.empty()) spclient = "gew4-spclient.spotify.com";  // regional fallback

    // GET storage-resolve with Bearer token only (no client-token needed).
    std::string url = "https://" + spclient +
                      "/storage-resolve/files/audio/interactive/"
                    + hex_encode(file_id);
    std::string auth_hdr = "Authorization: Bearer " + atk;

    std::vector<uint8_t> body;
    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, auth_hdr.c_str());
    CURL *curl = curl_easy_init();
    if (!curl) { curl_slist_free_all(hdrs); return {}; }
    curl_easy_setopt(curl, CURLOPT_URL,          url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,   hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char *p, size_t sz, size_t n, void *ud) -> size_t {
            auto *out = static_cast<std::vector<uint8_t> *>(ud);
            out->insert(out->end(), p, p + sz * n); return sz * n;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,    &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,      10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs); curl_easy_cleanup(curl);

    WHBLogPrintf("spirc: storage-resolve HTTP %ld body=%zu curl=%d (%s)",
                 code, body.size(), (int)rc, curl_easy_strerror(rc));
    if (rc != CURLE_OK || body.empty() || code != 200) return {};

    PRd rd{body.data(), body.data() + body.size()};
    std::string cdn_url; uint32_t f; uint8_t w;
    while (rd.next(f, w)) {
        if (f == 2 && w == 2) { rd.read_str(cdn_url); break; } else rd.skip(w);
    }
    WHBLogPrintf("spirc: cdn_url: %.70s", cdn_url.c_str());
    return cdn_url;
}

void Spirc::resolve_cdn(const std::vector<uint8_t> &file_id,
                         std::function<void(bool, std::string)> cb) {
    std::thread([this, file_id, cb = std::move(cb)]() mutable {
        OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0);
        std::string url = resolve_cdn_http(file_id);
        cb(!url.empty(), url);
    }).detach();
}

// ── Track metadata fetch ──────────────────────────────────────────────────────

void Spirc::fetch_track_metadata(const std::vector<uint8_t> &gid, int pos_ms) {
    std::string uri = "hm://metadata/3/track/" + hex_encode(gid);
    mercury_get(uri, [this, gid, pos_ms](bool ok, std::vector<std::vector<uint8_t>> parts) {
        if (!ok || parts.size() < 2) {
            WHBLogPrint("spirc: metadata fetch failed");
            return;
        }

        // parts[0] = MercuryHeader proto
        // parts[1] = Track proto
        const auto &body = parts[1];
        PRd rd{body.data(), body.data() + body.size()};

        std::string title;
        std::string artist;
        std::string art_url;
        std::vector<uint8_t> best_file_id;
        std::vector<uint8_t> best_file_gid = gid; // GID that owns best_file_id
        int best_fmt = -1; // prefer OGG_VORBIS_160=1, then 320=2, then 96=0
        int64_t duration_ms = 0;
        bool is_explicit = false;

        uint32_t f; uint8_t w;
        while (rd.next(f, w)) {
            switch (f) {
            case 2: rd.read_str(title); break;  // Track.name
            case 7: { uint64_t v; rd.vi(v); duration_ms = (int64_t)(int32_t)((v >> 1) ^ -(v & 1)); break; } // Track.duration (sint32 zigzag)
            case 3: {                            // Track.album
                PRd alb; rd.enter(alb);
                uint32_t af; uint8_t aw;
                uint64_t best_sz = 0;
                while (alb.next(af, aw)) {
                    if (af == 17) {             // Album.cover_group (ImageGroup, field 17)
                        PRd ig; alb.enter(ig);
                        uint32_t gf; uint8_t gw;
                        while (ig.next(gf, gw)) {
                            if (gf == 1) {      // ImageGroup.image
                                PRd img; ig.enter(img);
                                std::vector<uint8_t> fid; uint64_t sz = 0;
                                uint32_t imf; uint8_t imw;
                                while (img.next(imf, imw)) {
                                    if      (imf == 1) img.read_bytes(fid);
                                    else if (imf == 2) img.vi(sz); // size: DEFAULT=0 SMALL=1 LARGE=2 XLARGE=3
                                    else               img.skip(imw);
                                }
                                // Keep the largest size (highest enum value = largest resolution)
                                if (!fid.empty() && sz >= best_sz) {
                                    best_sz = sz;
                                    art_url = "https://i.scdn.co/image/" + hex_encode(fid);
                                }
                            } else ig.skip(gw);
                        }
                    } else alb.skip(aw);
                }
                break;
            }
            case 4: {                           // Track.artist (repeated ArtistWithRole)
                PRd art; rd.enter(art);
                std::string name;
                uint32_t af; uint8_t aw;
                while (art.next(af, aw)) {
                    if (af == 2) art.read_str(name);
                    else         art.skip(aw);
                }
                if (artist.empty() && !name.empty()) artist = name;
                break;
            }
            case 12: {                          // Track.file (AudioFile, repeated)
                PRd af; rd.enter(af);
                std::vector<uint8_t> fid; uint64_t fmt = 255;
                uint32_t ff; uint8_t fw;
                while (af.next(ff, fw)) {
                    if      (ff == 1) af.read_bytes(fid);
                    else if (ff == 2) af.vi(fmt);
                    else              af.skip(fw);
                }
                // Prefer OGG_VORBIS_160(1) > OGG_VORBIS_320(2) > OGG_VORBIS_96(0)
                if (!fid.empty()) {
                    int prio = (fmt == 1) ? 3 : (fmt == 2) ? 2 : (fmt == 0) ? 1 : 0;
                    if (prio > best_fmt) { best_fmt = prio; best_file_id = fid; }
                }
                break;
            }
            case 9: {                           // Track.explicit (bool varint)
                uint64_t v; rd.vi(v);
                if (v) is_explicit = true;
                break;
            }
            case 13: {                          // Track.alternative (repeated Track)
                // Alternative tracks have their own GID — must use it when requesting the key.
                PRd alt; rd.enter(alt);
                std::vector<uint8_t> alt_gid;
                std::vector<uint8_t> alt_best_fid;
                int alt_best_fmt = -1;
                uint32_t atf; uint8_t atw;
                while (alt.next(atf, atw)) {
                    if (atf == 1) {             // alternative.gid
                        alt.read_bytes(alt_gid);
                    } else if (atf == 12) {     // alternative.file
                        PRd af; alt.enter(af);
                        std::vector<uint8_t> fid; uint64_t fmt = 255;
                        uint32_t ff; uint8_t fw;
                        while (af.next(ff, fw)) {
                            if      (ff == 1) af.read_bytes(fid);
                            else if (ff == 2) af.vi(fmt);
                            else              af.skip(fw);
                        }
                        if (!fid.empty()) {
                            int prio = (fmt == 1) ? 3 : (fmt == 2) ? 2 : (fmt == 0) ? 1 : 0;
                            if (prio > alt_best_fmt) { alt_best_fmt = prio; alt_best_fid = fid; }
                        }
                    } else alt.skip(atw);
                }
                if (!alt_best_fid.empty() && alt_best_fmt > best_fmt) {
                    best_fmt     = alt_best_fmt;
                    best_file_id = alt_best_fid;
                    best_file_gid = alt_gid.size() == 16 ? alt_gid : gid;
                }
                break;
            }
            default: rd.skip(w); break;
            }
        }

        WHBLogPrintf("spirc: metadata '%s' / '%s' fmt=%d dur=%lldms explicit=%d",
                     title.c_str(), artist.c_str(), best_fmt, (long long)duration_ms, (int)is_explicit);
        if (duration_ms > 0) duration_ms_ = duration_ms;

        // Ensure the URI slot for the current track is populated.
        {
            size_t idx = playing_track_idx_;
            if (gid.size() == 16) {
                if (idx >= track_uris_.size()) {
                    track_uris_.resize(idx + 1);
                    track_gids_.resize(idx + 1);
                }
                if (track_uris_[idx].empty()) {
                    track_uris_[idx] = "spotify:track:" + gid_to_base62(gid);
                    track_gids_[idx] = gid;
                }
            }
        }

        // Re-push state now that duration (and possibly URI) are available.
        // The initial PUT at load time had duration=0; this corrects it.
        if (started_.load())
            put_connect_state_async(4 /* PLAYER_STATE_CHANGED */, playing_, pos_ms_, vol_pct_);

        if (callbacks_.on_track_changed)
            callbacks_.on_track_changed(title, artist, art_url, duration_ms_, is_explicit,
                                        gid_to_base62(gid));

        if (!best_file_id.empty() && callbacks_.on_file_ready)
            callbacks_.on_file_ready(best_file_id, best_file_gid, pos_ms);
    });
}

// Convert a ContextPage page_url to the Spotify URI used for context-resolve.
// e.g. "hm://artistplaycontext/v1/page/spotify/album/5LFz.../km_artist"
//   →  "spotify:album:5LFz..."
// Mirrors librespot's page_url_to_uri() in state/context.rs.
static std::string page_url_to_spotify_uri(const std::string &url) {
    std::string p = (url.size() > 5 && url.compare(0, 5, "hm://") == 0)
                    ? url.substr(5) : url;
    size_t sp = p.find("/spotify/");
    if (sp == std::string::npos) return {};
    std::string tail = p.substr(sp + 1);          // "spotify/TYPE/ID/..."
    size_t s1 = tail.find('/');                    // after "spotify"
    if (s1 == std::string::npos) return {};
    size_t s2 = tail.find('/', s1 + 1);           // after TYPE
    if (s2 == std::string::npos) return {};
    size_t s3 = tail.find('/', s2 + 1);           // after ID (may be npos)
    std::string type = tail.substr(s1 + 1, s2 - s1 - 1);
    std::string id   = (s3 != std::string::npos)
                       ? tail.substr(s2 + 1, s3 - s2 - 1)
                       : tail.substr(s2 + 1);
    if (type.empty() || id.empty()) return {};
    return "spotify:" + type + ":" + id;
}

// Parse all ContextTrack entries out of a serialised Context proto body.
// Handles multiple pages inline; returns page_url strings for deferred pages.
static void parse_context_body(
        const std::vector<uint8_t> &body,
        std::vector<std::string>            &out_uris,
        std::vector<std::vector<uint8_t>>   &out_gids,
        std::vector<std::string>            &out_uids,
        std::vector<std::string>            &out_page_urls)
{
    PRd rd{body.data(), body.data() + body.size()};
    uint32_t f; uint8_t w;
    while (rd.next(f, w)) {
        if (f == 5 && w == 2) {  // ContextPage (Context.pages = 5)
            PRd page; rd.enter(page);
            std::string page_url;
            std::vector<std::string>           pu;
            std::vector<std::vector<uint8_t>>  pg;
            std::vector<std::string>           pid;
            while (page.next(f, w)) {
                if (f == 1 && w == 2) {
                    page.read_str(page_url);   // page_url
                } else if (f == 4 && w == 2) { // ContextTrack
                    PRd tk; page.enter(tk);
                    std::string uri, uid; std::vector<uint8_t> gid;
                    while (tk.next(f, w)) {
                        if      (f == 1 && w == 2) tk.read_str(uri);
                        else if (f == 2 && w == 2) tk.read_bytes(gid);
                        else if (f == 3 && w == 2) tk.read_str(uid);
                        else                        tk.skip(w);
                    }
                    if (!uri.empty()) { pu.push_back(uri); pg.push_back(gid); pid.push_back(uid); }
                } else page.skip(w);
            }
            if (!pu.empty()) {
                for (size_t j = 0; j < pu.size(); j++) {
                    out_uris.push_back(pu[j]);
                    out_gids.push_back(j < pg.size()  ? pg[j]  : std::vector<uint8_t>{});
                    out_uids.push_back(j < pid.size() ? pid[j] : "");
                }
            } else if (!page_url.empty()) {
                out_page_urls.push_back(page_url);
            }
        } else rd.skip(w);
    }
}

void Spirc::fetch_context_tracks(const std::string &context_uri,
                                   const std::string &current_uri) {
    if (context_uri.empty()) return;
    if (context_uri.compare(0, 17, "spotify:playlist:") != 0 &&
        context_uri.compare(0, 14, "spotify:album:") != 0 &&
        context_uri.compare(0, 15, "spotify:artist:") != 0 &&
        context_uri.compare(0, 20, "spotify:collection:") != 0)
        return;

    std::string muri = "hm://context-resolve/v1/" + context_uri;
    mercury_get(muri, [this, context_uri, current_uri](bool ok,
                std::vector<std::vector<uint8_t>> parts) {
        if (!ok || parts.size() < 2 || parts[1].empty()) {
            WHBLogPrintf("spirc: ctx_resolve failed ok=%d parts=%zu", (int)ok, parts.size());
            return;
        }
        if (context_uri_ != context_uri) return;
        // Drop stale responses: if the playing track changed since we fired the
        // request, the resolved index would be wrong and would cause oscillation.
        if (current_track_uri_ != current_uri) return;

        std::vector<std::string>          uris;
        std::vector<std::vector<uint8_t>> gids;
        std::vector<std::string>          uids;
        std::vector<std::string>          page_urls;
        parse_context_body(parts[1], uris, gids, uids, page_urls);

        if (uris.empty()) {
            WHBLogPrintf("spirc: ctx_resolve: no inline tracks (page_urls=%zu)", page_urls.size());
            // still fetch deferred pages
        } else {
            bool found = false;
            size_t new_idx = 0;
            for (size_t k = 0; k < uris.size(); ++k) {
                if (uris[k] == current_uri) { new_idx = k; found = true; break; }
            }

            WHBLogPrintf("spirc: ctx_resolve: %zu tracks found=%d idx=%zu deferred_pages=%zu",
                         uris.size(), (int)found, new_idx, page_urls.size());

            if (found) {
                track_uris_        = uris;
                track_gids_        = gids;
                track_uids_        = uids;
                playing_track_idx_ = new_idx;

                if (started_.load())
                    put_connect_state_async(4, playing_, pos_ms_, vol_pct_);
            } else {
                // Current track not in first page — likely a later deferred page.
                // Keep the cluster-window data; deferred fetches will append the rest.
                WHBLogPrintf("spirc: ctx_resolve: current track not in first page, awaiting deferred pages");
            }
        }

        // Fetch pages that only had a page_url (deferred/paginated pages)
        for (const auto &pu : page_urls) {
            std::string resolved = page_url_to_spotify_uri(pu);
            if (!resolved.empty())
                fetch_context_page(resolved, context_uri);
            else
                WHBLogPrintf("spirc: ctx_resolve: unrecognised page_url '%s'", pu.c_str());
        }
    });
}

void Spirc::fetch_context_page(const std::string &page_ctx_uri,
                                 const std::string &context_uri) {
    std::string muri = "hm://context-resolve/v1/" + page_ctx_uri;
    mercury_get(muri, [this, page_ctx_uri, context_uri](bool ok,
                std::vector<std::vector<uint8_t>> parts) {
        if (!ok || parts.size() < 2 || parts[1].empty()) {
            WHBLogPrintf("spirc: ctx_page '%s' failed", page_ctx_uri.c_str());
            return;
        }
        if (context_uri_ != context_uri) return;

        std::vector<std::string>          uris;
        std::vector<std::vector<uint8_t>> gids;
        std::vector<std::string>          uids;
        std::vector<std::string>          more_urls; // nested pages (e.g. artist → albums)
        parse_context_body(parts[1], uris, gids, uids, more_urls);

        if (uris.empty()) {
            WHBLogPrintf("spirc: ctx_page '%s': no tracks", page_ctx_uri.c_str());
            return;
        }

        WHBLogPrintf("spirc: ctx_page '%s': +%zu tracks (total now %zu)",
                     page_ctx_uri.c_str(), uris.size(), track_uris_.size() + uris.size());

        for (auto &u : uris) track_uris_.push_back(u);
        for (auto &g : gids) track_gids_.push_back(g);
        for (auto &u : uids) track_uids_.push_back(u);

        if (started_.load())
            put_connect_state_async(4, playing_, pos_ms_, vol_pct_);

        // Handle nested deferred pages (e.g. artist context has page_urls per album)
        for (const auto &pu : more_urls) {
            std::string resolved = page_url_to_spotify_uri(pu);
            if (!resolved.empty())
                fetch_context_page(resolved, context_uri);
        }
    });
}

} // namespace Connect
