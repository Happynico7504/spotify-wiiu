#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include "dealer.h"

namespace Connect {

class AP;

// Spirc v1 — Spotify Remote Procedure Call protocol.
//
// Subscribes to:
//   hm://remote/3/user/{username}/              — broadcast channel
//   hm://remote/3/user/{username}/{device_id}/  — targeted channel
//
// Sends outbound Frames (Hello on start, Notify on every state change)
// to hm://remote/3/user/{username}/ over Mercury (0xB2 MercuryReq).
//
// Metadata for the current track is fetched asynchronously via
// hm://metadata/3/track/{gid_hex} and delivered through on_track_changed
// and on_file_ready once it arrives.

class Spirc {
public:
    struct Callbacks {
        // Fired immediately when a Load/Play command arrives.
        std::function<void(const std::string &track_uri, int pos_ms)> on_play;

        // Fired after metadata fetch.  file_id (20 B) and track_gid (16 B) are both
        // required by the AP's RequestKey packet; pos_ms is the start position.
        std::function<void(const std::vector<uint8_t> &file_id,
                           const std::vector<uint8_t> &track_gid,
                           int pos_ms)> on_file_ready;

        // Fired whenever the Spirc playing state changes.
        // playing=true  → Spotify said play/resume
        // playing=false → Spotify said pause
        std::function<void(bool playing, int pos_ms)> on_playing_changed;

        // Fired when shuffle or repeat changes (host toggle or local toggle).
        // repeat_mode: 0=off, 1=repeat-context, 2=repeat-track
        std::function<void(bool shuffle, int repeat_mode)> on_options_changed;

        // Fired on Seek command.
        std::function<void(int pos_ms)> on_seek;

        // Fired on Volume command (volume_pct: 0–100).
        std::function<void(int vol_pct)> on_volume;

        // Fired after metadata fetch — display info for the track.
        std::function<void(const std::string &title,
                           const std::string &artist,
                           const std::string &art_url,
                           int64_t duration_ms,
                           bool is_explicit,
                           const std::string &track_id)> on_track_changed;
    };

    // ap must outlive Spirc.
    Spirc(AP *ap, const std::string &username, const std::string &device_name,
          const std::string &device_id);
    ~Spirc();

    // Subscribe to Mercury topics and send Hello frame.
    // Call after AP::connect() returns true.
    bool start(Callbacks cb);
    void stop();

    // Feed every AP packet here; Spirc filters MercuryReq/Event.
    void on_packet(uint8_t cmd, std::vector<uint8_t> payload);

    // Update Spirc's internal state so outbound Notify frames stay accurate.
    void notify(bool playing, int pos_ms, int vol_pct);

    // Local skip/seek — same effect as receiving Next/Prev/Seek from the app.
    void skip(bool next_track);
    void seek_to(int pos_ms);

    // Toggle shuffle / repeat and push updated state to Spotify.
    void toggle_shuffle();
    void toggle_repeat();

    bool shuffle() const { return shuffle_; }
    bool repeat_context() const { return repeat_; }
    bool repeat_track()   const { return repeat_track_; }
    // 0=off, 1=repeat-context, 2=repeat-track  (for display)
    int  repeat_mode()    const { return repeat_track_ ? 2 : (repeat_ ? 1 : 0); }

    // Signal natural track completion (is_paused=false, position=end).
    // Use instead of notify(false,...) when the audio pipeline drains naturally.
    void notify_track_end(int vol_pct);

    // Re-play the current track from the beginning without advancing the index.
    // Used by Player when repeat is on and a track finishes.
    void replay_current();

    // Async Mercury GET.  cb(ok, parts) is called from the AP recv thread when
    // the response arrives.  Exposed so Player can resolve CDN URLs etc.
    void mercury_get(const std::string &uri,
                     std::function<void(bool, std::vector<std::vector<uint8_t>>)> cb);

    // Convenience: resolve the Spotify CDN URL for an audio file.
    // Sends hm://storage-resolve/files/audio/interactive/{file_id_hex} and
    // calls cb(ok, cdn_url) when the StorageResolveResponse arrives.
    void resolve_cdn(const std::vector<uint8_t> &file_id,
                     std::function<void(bool, std::string)> cb);

private:
    // ── Mercury outbound ─────────────────────────────────────────────────────
    void send_hello();
    void send_notify(bool playing, int pos_ms, int vol_pct);
    void mercury_subscribe(const std::string &uri);
    void mercury_send(const std::string &uri, const std::vector<uint8_t> &frame);

    // ── Mercury inbound ──────────────────────────────────────────────────────
    void handle_mercury_response(const std::vector<uint8_t> &payload);
    void handle_mercury_event(const std::vector<uint8_t> &payload);
    void handle_frame(const std::vector<uint8_t> &frame_bytes);

    // ── Dealer inbound ───────────────────────────────────────────────────────
    void handle_dealer_message(const std::string &uri,
                               const std::vector<uint8_t> &payload);
    bool handle_dealer_request(const std::string &message_ident,
                               const std::string &cmd_json);

    // ── Metadata ─────────────────────────────────────────────────────────────
    void fetch_track_metadata(const std::vector<uint8_t> &gid, int pos_ms);
    // Fetch the full ordered track list for a context URI via Mercury.
    // Called when a cluster-update / transfer gives us only the current track.
    void fetch_context_tracks(const std::string &context_uri,
                               const std::string &current_uri);
    // Append one additional context page to the track list.
    // Called for page_url entries discovered in fetch_context_tracks.
    void fetch_context_page(const std::string &page_ctx_uri,
                             const std::string &context_uri);

    // ── spclient token acquisition (HTTP only, no AP Mercury) ────────────────
    std::string resolve_cdn_http(const std::vector<uint8_t> &file_id);

    // ── connect-state v1 (PutStateRequest over HTTP) ─────────────────────────
    // Build connect-state proto blobs (called on handler thread, no HTTP).
    std::vector<uint8_t> build_player_state(bool playing, int pos_ms, int64_t now_ms);
    std::vector<uint8_t> build_device_info_cs(int vol_pct);
    // Assemble PutStateRequest bytes on the calling thread, then PUT in a background thread.
    void put_connect_state_async(uint32_t reason, bool playing, int pos_ms, int vol_pct);
    // Blocking HTTP PUT — run only from detached threads.
    long put_connect_state_sync(std::vector<uint8_t> psr, uint32_t reason, bool playing);

    // ── Frame / wire builders ─────────────────────────────────────────────────
    std::vector<uint8_t> build_frame(uint32_t msg_type, bool playing,
                                     int pos_ms, int vol_pct);
    std::vector<uint8_t> build_device_state(bool active, int vol_pct);

    // Parse multi-part Mercury payload → list of raw part blobs (skips seq+flags+count header)
    static std::vector<std::vector<uint8_t>> parse_parts(const std::vector<uint8_t> &payload);

    // ── state ─────────────────────────────────────────────────────────────────
    AP         *ap_;
    std::string username_;
    std::string device_name_;
    std::string device_id_;

    std::mutex            seq_mu_;     // guards mercury_seq_
    uint32_t              mercury_seq_ = 0;
    std::atomic<uint32_t> frame_seq_{0};

    bool playing_  = false;
    int  pos_ms_   = 0;
    int  vol_pct_  = 100;

    // Player state echoed back in Notify frames (set on Load)
    std::string                        context_uri_;
    std::string                        current_track_uri_;      // guard against stale fetch_context_tracks responses
    uint32_t                           playing_track_idx_ = 0;  // offset in local window
    uint32_t                           abs_track_idx_     = 0;  // absolute index in full playlist
    std::vector<std::vector<uint8_t>>  track_gids_;
    std::vector<std::string>           track_uris_;
    std::vector<std::string>           track_uids_;  // per-slot UID from context metadata
    int64_t                            state_update_id_   = 0;
    int64_t                            duration_ms_       = 0;

    Callbacks         callbacks_;
    std::atomic<bool> started_{false};
    bool shuffle_       = false;
    bool repeat_        = false;   // repeating_context
    bool repeat_track_  = false;   // repeating_track

    // Pending Mercury GET requests: mercury_seq → callback
    struct PendingReq {
        std::function<void(bool, std::vector<std::vector<uint8_t>>)> cb;
    };
    std::map<uint32_t, PendingReq> pending_;
    std::mutex                     pending_mu_;

    // Cached access_token from login5.spotify.com — fetched lazily on first CDN resolve.
    std::string  access_token_;
    std::mutex   token_mu_;

    // connect-state v1 session state
    std::string       connection_id_;          // from Dealer WebSocket handshake
    std::mutex        conn_mu_;
    std::string       spclient_host_;          // cached from apresolve
    std::mutex        spclient_mu_;
    int64_t           started_playing_at_ms_ = 0;  // Unix ms when device first became active
    std::atomic<bool> helo_done_{false};       // true after first /connect/state PUT succeeds
    int64_t           last_state_push_ms_ = 0; // Unix ms of most-recent put_connect_state_async
    std::atomic<int>  push_inflight_{0};       // 0=idle, 1=PUT inflight (at most one at a time)
    std::atomic<bool> dirty_push_{false};      // state changed while a PUT was inflight

    // Dealer WebSocket — provides the connection_id for connect-state
    Dealer            dealer_;
    std::thread       dealer_init_thread_;     // fetches token then starts dealer_
};

} // namespace Connect
