#pragma once
#include <cstdint>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include "../discovery/zeroconf.h"
#include "../ui/display.h"

// Forward declarations — full headers pulled in player.cpp only
namespace Connect { class AP; class Spirc; class AudioPipeline; }

namespace Connect {

// Top-level state machine.  player.run() is the main loop.
//
//   WaitingForUser  ← Zeroconf running, mDNS announcing, no credentials yet
//         ↓           (Spotify app taps the device)
//   Connecting      ← AP TCP connect, Shannon handshake, login
//         ↓
//   Ready           ← Spirc subscribed, waiting for play command
//         ↓
//   Playing         ← audio pipeline active, chunks fetching+decoding
//
// Any state can return to WaitingForUser if the AP disconnects or
// the user revokes access.

class Player {
public:
    Player();
    ~Player();

    void run();   // blocks until WHBProcIsRunning() is false

private:
    void on_credentials(Discovery::Credentials creds);

    // Spirc callbacks (invoked from AP recv thread)
    void on_play(const std::string &track_uri, int position_ms);
    void on_file_ready(const std::vector<uint8_t> &file_id,
                       const std::vector<uint8_t> &track_gid,
                       int position_ms);
    void on_seek(int position_ms);
    void on_volume(int volume_pct);
    void on_track_changed(const std::string &title, const std::string &artist,
                          const std::string &art_url, int64_t duration_ms, bool is_explicit);

    // Receives every AP packet not handled internally by AP (AesKey, Mercury, …)
    void on_ap_packet(uint8_t cmd, std::vector<uint8_t> payload);

    // GamePad input (called from main loop, vpad already read)
    void handle_buttons(uint32_t trigger);
    // Wiimote input (called from main loop, kpad already read)
    void handle_wiimote_buttons(uint32_t trigger);
    // Pro Controller input (called from main loop, kpad.pro already read)
    void handle_pro_buttons(uint32_t trigger);

    void kill_connection();   // drop AP/Spirc after background timeout

    enum class State { WaitingForUser, Connecting, Ready, Playing };
    std::atomic<State>    state_{State::WaitingForUser};
    std::atomic<int32_t>  background_since_s_{0};  // 0 = foreground; nonzero = Unix s entered bg

    Discovery::Zeroconf              zeroconf_;
    std::unique_ptr<AP>              ap_;
    std::unique_ptr<Spirc>           spirc_;
    std::unique_ptr<AudioPipeline>   audio_;
    UI::Display                      display_;

    int            volume_          = 100;
    bool           spirc_playing_   = false;
    int            track_dur_ms_    = 0;
    bool           track_explicit_  = false;
    bool           crystal_enabled_ = false;
    int            crystal_strength_= 5;     // 1–10

    // Pending audio start state (CDN URL arrives first; AES key comes second)
    std::mutex              aes_mu_;
    uint32_t                aes_seq_        = 0;
    std::string             pending_cdn_url_;
    std::vector<uint8_t>    pending_file_id_;
    int                     pending_pos_ms_ = 0;

    // Serialises credential processing — drops duplicate addUser pushes that
    // arrive while we're mid-handshake (Spotify app often retries quickly).
    std::mutex  connect_mu_;
    std::thread connect_thread_;
};

} // namespace Connect
