#pragma once
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdint>
#include <mbedtls/dhm.h>

namespace Discovery {

// Credentials extracted from Zeroconf — handed to the AP layer for login.
// auth_type mirrors Spotify's AuthenticationType proto enum:
//   0x00 = AUTHENTICATION_USER_PASS
//   0x01 = AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS  (old blob flow)
//   0x03 = AUTHENTICATION_SPOTIFY_TOKEN               (new OAuth token / loginId flow)
//   0x04 = AUTHENTICATION_FACEBOOK_TOKEN
struct Credentials {
    std::string username;
    std::string device_id;  // 40-hex-char Wii U device ID for system_info
    std::vector<uint8_t> auth_data;
    uint8_t auth_type = 0x01;
};

// Zeroconf runs two background threads:
//   mdns_thread_  — sends periodic mDNS announcements and answers PTR/SRV/TXT
//                   queries on 224.0.0.251:5353 for _spotify-connect._tcp
//   http_thread_  — minimal TCP HTTP server; handles:
//                     GET  /zc?action=getInfo  → JSON device info + DH public key
//                     POST /zc?action=addUser  → decrypt blob → call on_creds
//
// The Spotify app on the user's phone/PC discovers the device via mDNS, then
// POSTs encrypted credentials.  No browser, no QR code.

class Zeroconf {
public:
    Zeroconf(const std::string &device_name, const std::string &device_id);
    ~Zeroconf();

    // Start both threads.  on_creds is called (once, from http_thread_) when a
    // Spotify client successfully pushes credentials.  on_error (optional) is
    // called with a short human-readable message if credentials cannot be loaded.
    void start(std::function<void(Credentials)> on_creds,
               std::function<void(std::string)> on_error = {});
    void stop();

    const std::string &device_name() const { return device_name_; }
    const std::string &device_id()   const { return device_id_; }

    // Call after AP login succeeds — written into every subsequent getInfo response.
    void set_active_user(const std::string &username) { active_user_ = username; }

private:
    // ── mDNS ─────────────────────────────────────────────────────────────────
    void mdns_thread_fn();

    // Build and send a full mDNS announcement (PTR + SRV + TXT + A records)
    // for _spotify-connect._tcp.local on the multicast socket.
    void mdns_announce(int sock, uint32_t local_ip);

    // ── HTTP server ──────────────────────────────────────────────────────────
    void http_thread_fn();
    void handle_client(int fd);

    // GET /zc?action=getInfo → JSON
    std::string build_info_json() const;

    // POST /zc?action=addUser
    // clientKey: base64-decoded DH public key from the Spotify app
    // blob:      base64-decoded encrypted credential blob
    // Returns true and fills `out` on success.
    bool decrypt_blob(const std::string &username,
                      const std::vector<uint8_t> &client_key,
                      const std::vector<uint8_t> &blob,
                      Credentials &out);

    // ── helpers ──────────────────────────────────────────────────────────────
    // Read HTTP request from fd, return body (blocking, short timeout).
    std::string http_read(int fd);
    // Write HTTP response to fd.
    void http_respond(int fd, int status, const std::string &content_type,
                      const std::string &body);

    // ── state ─────────────────────────────────────────────────────────────────
    std::string device_name_;
    std::string device_id_;          // 40-hex-char unique ID (sha1 of MAC or random)
    std::string active_user_;        // canonical username after AP login; empty until then
    int         http_port_ = 4070;   // port advertised in mDNS SRV record

    // DH-1024 key pair (generated once in constructor, public key sent in getInfo)
    mbedtls_dhm_context dhm_;
    std::string         public_key_b64_;  // base64(DH public key), cached

    std::function<void(Credentials)> on_creds_;
    std::function<void(std::string)> on_error_;
    std::thread         mdns_thread_;
    std::thread         http_thread_;
    std::atomic<bool>   stop_{false};
};

} // namespace Discovery
