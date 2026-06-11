#include "player.h"
#include "ap.h"
#include "spirc.h"
#include "audio.h"
#include "../olv/olv.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>

#include <vpad/input.h>
#include <padscore/kpad.h>
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/sdcard.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <proc_ui/procui.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

namespace Connect {

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string make_device_id() {
    auto *entropy  = new mbedtls_entropy_context;
    auto *ctr_drbg = new mbedtls_ctr_drbg_context;
    mbedtls_entropy_init(entropy);
    mbedtls_ctr_drbg_init(ctr_drbg);
    mbedtls_ctr_drbg_seed(ctr_drbg, mbedtls_entropy_func, entropy,
                          reinterpret_cast<const uint8_t *>("dev"), 3);
    uint8_t raw[20];
    mbedtls_ctr_drbg_random(ctr_drbg, raw, sizeof(raw));
    mbedtls_ctr_drbg_free(ctr_drbg);
    mbedtls_entropy_free(entropy);
    delete ctr_drbg; delete entropy;

    static const char hx[] = "0123456789abcdef";
    std::string s; s.reserve(40);
    for (uint8_t b : raw) { s += hx[b >> 4]; s += hx[b & 0xF]; }
    return s;
}


// ── Constructor / destructor ──────────────────────────────────────────────────

Player::Player()
    : zeroconf_("Wii U", make_device_id()),
      ap_(std::make_unique<AP>()),
      audio_(std::make_unique<AudioPipeline>())
{}

Player::~Player() {
    if (spirc_) spirc_->stop();
    if (ap_)    ap_->disconnect();
}

// ── Main loop ─────────────────────────────────────────────────────────────────

void Player::run() {
    OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU1);
    // Font is embedded in the binary; pass nullptr so display falls back to it.
    // If a font file exists on the SD card it will be preferred automatically.
    const char *font_path = nullptr;
    if (const char *sd = WHBGetSdCardMountPath()) {
        static char sd_font[256];
        snprintf(sd_font, sizeof(sd_font),
                 "%s/wiiu/apps/spotify-wiiu/font.ttf", sd);
        if (FILE *f = fopen(sd_font, "rb")) { fclose(f); font_path = sd_font; }
    }
    KPADInit();
    WPADEnableURCC(TRUE);  // must be called explicitly to allow Pro Controllers to connect

    // Kill connection after this many seconds in the background (HOME menu, etc.)
    static constexpr int32_t BG_KILL_SECS = 180;

    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE,
        [](void *ctx) -> uint32_t {
            // Use OSGetSystemTick()-based seconds — time() may return 0 on Wii U
            // homebrew if the RTC hasn't been mapped to Unix epoch.
            int32_t s = (int32_t)(OSGetSystemTick() / OSSecondsToTicks(1));
            if (s == 0) s = 1;  // keep non-zero sentinel
            static_cast<Player*>(ctx)->background_since_s_.store(s);
            WHBLogPrint("player: entered background");
            return 0;
        }, this, 100);
    ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE,
        [](void *ctx) -> uint32_t {
            static_cast<Player*>(ctx)->background_since_s_.store(0);
            WHBLogPrint("player: returned to foreground");
            return 0;
        }, this, 100);

    if (!display_.init(font_path))
        WHBLogPrint("player: display init failed");
    display_.set_audio(audio_.get());
    display_.set_waiting();

    // Initialise OLV in background (blocks on network; detects Roséverse).
    olv_init_thread_ = std::thread([this] {
        OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0);
        OLV::init();
    });

    if (!audio_->init())
        WHBLogPrint("player: audio init failed");

    audio_->on_track_end = [this] {
        spirc_playing_ = false;
        if (spirc_) {
            if (spirc_->repeat_track())
                spirc_->replay_current();
            else
                spirc_->notify_track_end(volume_);
        }
        state_.store(State::Ready);
    };

    zeroconf_.start([this](Discovery::Credentials creds) {
        // Must not block the Zeroconf HTTP thread, so we spawn a separate thread.
        // Release the previous handle (detach if still running — ap_->disconnect()
        // will unblock it during cleanup, and zeroconf_.stop()'s ~200 ms join gives
        // it time to exit before Player is destroyed).
        if (connect_thread_.joinable())
            connect_thread_.detach();
        connect_thread_ = std::thread([this, creds = std::move(creds)]() mutable {
            OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0);
            on_credentials(std::move(creds));
        });
    });

    WHBLogPrint("player: waiting for Spotify app...");

    // WHBProcIsRunning() blocks while in background, so the main loop never
    // reaches the kill check while HOME menu is open. Run it on its own thread.
    std::atomic<bool> watchdog_stop{false};
    std::thread watchdog([&]() {
        OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0);
        while (!watchdog_stop.load()) {
            for (int i = 0; i < 5 && !watchdog_stop.load(); ++i)
                OSSleepTicks(OSSecondsToTicks(1));
            int32_t bg = background_since_s_.load();
            int32_t now_s = (int32_t)(OSGetSystemTick() / OSSecondsToTicks(1));
            if (bg != 0 && now_s - bg >= BG_KILL_SECS
                        && state_.load() != State::WaitingForUser) {
                WHBLogPrintf("player: background timeout (%ds) — dropping connection",
                             BG_KILL_SECS);
                background_since_s_.store(0);
                kill_connection();
            }
        }
    });

    VPADStatus    vpad{};
    VPADReadError verr{};
    uint64_t      last_notify_tick = 0;

    while (WHBProcIsRunning()) {
        display_.render();

        VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);
        if (verr == VPAD_READ_SUCCESS)
            handle_buttons(vpad.trigger);

        KPADStatus kpad{};
        KPADError  kerr{};
        for (int ch = 0; ch < 4; ++ch) {
            if (KPADReadEx((KPADChan)ch, &kpad, 1, &kerr) <= 0 || kerr != KPAD_ERROR_OK)
                continue;
            if (kpad.extensionType == WPAD_EXT_PRO_CONTROLLER) {
                // If the data format hasn't been switched yet, do it now and wait
                // one frame for KPAD to start delivering valid pro.trigger data.
                if (kpad.format != WPAD_FMT_PRO_CONTROLLER) {
                    WPADSetDataFormat((WPADChan)ch, WPAD_FMT_PRO_CONTROLLER);
                } else {
                    handle_pro_buttons(kpad.pro.trigger);
                }
            } else {
                handle_wiimote_buttons(kpad.trigger);
            }
        }

        // Send position Notify every 1 s so the Spotify app stays in sync.
        // Fire whenever we are in Playing state, not only when audio is
        // actively decoding — buffering/track-switch gaps must not go silent.
        if (spirc_playing_) {
            if (audio_->is_playing()) {
                int pos = audio_->position_ms();
                display_.set_progress(pos, track_dur_ms_, true);
            }
            uint64_t now = OSGetSystemTick();
            if (spirc_ && now - last_notify_tick >= OSSecondsToTicks(1)) {
                spirc_->notify(true, audio_->position_ms(), volume_);
                last_notify_tick = now;
            }
        }

        // Auto-advance OLV card every 5 s while visible
        if (OLV::is_available()) {
            uint64_t now = OSGetSystemTick();
            if (now - olv_last_advance_ >= OSSecondsToTicks(5)) {
                std::lock_guard<std::mutex> lk(olv_mu_);
                if (olv_card_visible_ && !olv_posts_.empty()) {
                    olv_last_advance_ = now;
                    olv_post_idx_ = (olv_post_idx_ + 1) % olv_posts_.size();
                    if (olv_post_idx_ == 0 && !olv_fetch_thread_.joinable())
                        olv_fetch_thread_ = std::thread([this] {
                            OSSetThreadAffinity(OSGetCurrentThread(),
                                                OS_THREAD_ATTRIB_AFFINITY_CPU0);
                            olv_fetch(OLV::COMMUNITY_ID);
                        });
                    else
                        olv_show_current();
                }
            }
        }

        OSSleepTicks(OSMillisecondsToTicks(16));
    }

    watchdog_stop.store(true);
    watchdog.join();

    // Disconnect AP first — shutdown(fd_) unblocks any recv_exact in the
    // connect thread, causing it to exit quickly.
    // kill_connection() may have already cleaned up ap_ and connect_thread_.
    if (ap_) ap_->disconnect();
    if (connect_thread_.joinable()) connect_thread_.join();
    zeroconf_.stop();
    if (spirc_) { spirc_->stop(); spirc_.reset(); }
    audio_->shutdown();
    display_.shutdown();

    // OLV teardown — join threads then shut down the module.
    if (olv_fetch_thread_.joinable()) olv_fetch_thread_.join();
    if (olv_init_thread_.joinable())  olv_init_thread_.join();
    OLV::shutdown();
}

// ── Credentials — connect to AP ───────────────────────────────────────────────

void Player::on_credentials(Discovery::Credentials creds) {
    // Drop duplicate pushes — the app often retries addUser while we're still
    // mid-handshake.  The mutex ensures only one connect attempt runs at a time.
    std::unique_lock<std::mutex> lk(connect_mu_, std::try_to_lock);
    if (!lk) {
        WHBLogPrint("player: ignoring duplicate credentials (already connecting)");
        return;
    }
    WHBLogPrintf("player: credentials for '%s'", creds.username.c_str());
    state_.store(State::Connecting);

    // Clean up any previous connection (join defunct recv thread, close fd)
    if (spirc_) { spirc_->stop(); spirc_.reset(); }
    ap_->disconnect();
    ap_ = std::make_unique<AP>();

    AP::Callbacks acb;
    acb.on_packet     = [this](uint8_t cmd, std::vector<uint8_t> pl) {
        on_ap_packet(cmd, std::move(pl));
    };
    acb.on_disconnect = [this] {
        WHBLogPrint("player: AP disconnected");
        state_.store(State::WaitingForUser);
        audio_->stop();
        spirc_.reset();
        display_.set_waiting();
    };

    if (!ap_->connect(creds, std::move(acb))) {
        WHBLogPrint("player: AP connect failed");
        state_.store(State::WaitingForUser);
        display_.set_waiting();
        return;
    }

    // AP connected — start Spirc
    spirc_ = std::make_unique<Spirc>(ap_.get(),
                                     ap_->canonical_username(),
                                     "Wii U",
                                     zeroconf_.device_id());

    Spirc::Callbacks scb;
    scb.on_play = [this](const std::string &uri, int pos) {
        on_play(uri, pos);
    };
    scb.on_file_ready = [this](const std::vector<uint8_t> &fid,
                                const std::vector<uint8_t> &gid,
                                int pos) {
        on_file_ready(fid, gid, pos);
    };
    scb.on_playing_changed = [this](bool playing, int pos_ms) {
        spirc_playing_ = playing;
        if (playing) {
            audio_->resume();
            display_.set_progress(pos_ms, track_dur_ms_, true);
        } else {
            audio_->pause();
            display_.set_progress(audio_->position_ms(), track_dur_ms_, false);
        }
    };
    scb.on_options_changed = [this](bool sh, int rep) {
        display_.set_shuffle(sh);
        display_.set_repeat(rep);
    };
    scb.on_seek         = [this](int pos) { on_seek(pos); };
    scb.on_volume       = [this](int vol) { on_volume(vol); };
    scb.on_track_changed = [this](const std::string &t,
                                  const std::string &a,
                                  const std::string &u,
                                  int64_t dur,
                                  bool expl,
                                  const std::string &id) {
        on_track_changed(t, a, u, dur, expl, id);
    };

    spirc_->start(std::move(scb));
    state_.store(State::Ready);
    const std::string &canon = ap_->canonical_username();
    zeroconf_.set_active_user(canon);
    WHBLogPrintf("player: connected as '%s'", canon.c_str());
}

// ── Background-timeout disconnect ────────────────────────────────────────────

void Player::kill_connection() {
    // Stop audio first so on_track_end can't fire after spirc_ is gone.
    audio_->stop();
    spirc_playing_ = false;
    track_dur_ms_  = 0;

    // Stop Spirc (sends Goodbye, tears down Dealer thread).
    if (spirc_) { spirc_->stop(); spirc_.reset(); }

    // Disconnect AP (unblocks the recv thread).
    if (ap_) { ap_->disconnect(); ap_.reset(); }

    // Join the connect thread so any in-progress on_credentials finishes cleanly.
    if (connect_thread_.joinable()) connect_thread_.join();

    state_.store(State::WaitingForUser);
    display_.set_waiting();
    WHBLogPrint("player: connection killed — waiting for reconnect");
}

// ── Spirc callbacks ───────────────────────────────────────────────────────────

void Player::on_play(const std::string &track_uri, int position_ms) {
    WHBLogPrintf("player: load '%s' @%dms", track_uri.c_str(), position_ms);
    audio_->stop();
    spirc_playing_ = true;
    state_.store(State::Playing);
    // Audio starts when on_file_ready fires and we receive the AES key
}

void Player::on_file_ready(const std::vector<uint8_t> &file_id,
                            const std::vector<uint8_t> &track_gid,
                            int pos_ms) {
    WHBLogPrint("player: file ready — resolving CDN URL");

    // Step 1: resolve CDN URL via Mercury storage-resolve
    spirc_->resolve_cdn(file_id, [this, file_id, track_gid, pos_ms]
                        (bool ok, std::string cdn_url) {
        if (!ok || cdn_url.empty()) {
            WHBLogPrint("player: CDN resolve failed");
            return;
        }
        WHBLogPrintf("player: CDN URL resolved, requesting AES key");

        // Step 2: store CDN URL and request AES key from AP
        uint32_t seq;
        {
            std::lock_guard<std::mutex> lk(aes_mu_);
            seq              = aes_seq_++;
            pending_cdn_url_ = cdn_url;
            pending_file_id_ = file_id;
            pending_pos_ms_  = pos_ms;
        }

        // RequestKey payload: file_id(20) + track_gid(16) + seq(4 BE) + zero(2)
        // Matches librespot audio_key.rs send_key_request byte order.
        std::vector<uint8_t> pkt;
        pkt.insert(pkt.end(), file_id.begin(),   file_id.end());
        pkt.insert(pkt.end(), track_gid.begin(), track_gid.end());
        pkt.push_back((seq >> 24) & 0xFF); pkt.push_back((seq >> 16) & 0xFF);
        pkt.push_back((seq >>  8) & 0xFF); pkt.push_back( seq        & 0xFF);
        pkt.push_back(0x00); pkt.push_back(0x00);

        // Debug: log file_id and track_gid
        {
            static const char hx[] = "0123456789abcdef";
            std::string fid_hex, gid_hex;
            for (uint8_t b : file_id)  { fid_hex += hx[b>>4]; fid_hex += hx[b&0xF]; }
            for (uint8_t b : track_gid){ gid_hex += hx[b>>4]; gid_hex += hx[b&0xF]; }
            WHBLogPrintf("player: RequestKey file=%s gid=%s seq=%u",
                         fid_hex.c_str(), gid_hex.c_str(), seq);
        }

        ap_->send_packet(Cmd::RequestKey, pkt);
    });
}

void Player::on_ap_packet(uint8_t cmd, std::vector<uint8_t> payload) {
    if (cmd == Cmd::AesKey) {
        // Payload: [4-byte seq BE][16-byte key]
        if (payload.size() < 20) {
            WHBLogPrint("player: AesKey payload too short");
            return;
        }
        std::vector<uint8_t> key(payload.begin() + 4, payload.begin() + 20);

        std::string              cdn_url;
        std::vector<uint8_t>     file_id;
        int pos;
        {
            std::lock_guard<std::mutex> lk(aes_mu_);
            cdn_url = pending_cdn_url_;
            file_id = pending_file_id_;
            pos     = pending_pos_ms_;
        }
        if (!cdn_url.empty()) {
            WHBLogPrint("player: AES key received — starting audio");
            audio_->set_volume(volume_);
            audio_->play(cdn_url, key, file_id, pos);
        }
    } else if (cmd == Cmd::AesKeyError) {
        uint16_t ec = (payload.size() >= 6)
            ? (uint16_t)((payload[4] << 8) | payload[5]) : 0xFFFF;
        WHBLogPrintf("player: AES key error from AP code=0x%04X — skipping track", (unsigned)ec);
        if (spirc_) spirc_->skip(true);
    } else if (spirc_) {
        // Mercury responses (0xB2), events (0xB5), acks (0xB6), etc.
        spirc_->on_packet(cmd, std::move(payload));
    }
}


void Player::on_seek(int pos_ms) {
    audio_->seek(pos_ms);
}

void Player::on_volume(int vol_pct) {
    volume_ = vol_pct;
    audio_->set_volume(vol_pct);
    display_.set_volume(vol_pct);
}

void Player::on_track_changed(const std::string &title, const std::string &artist,
                               const std::string &art_url, int64_t duration_ms, bool is_explicit,
                               const std::string &track_id) {
    track_dur_ms_   = (int)std::min(duration_ms, (int64_t)INT_MAX);
    track_title_    = title;
    track_artist_   = artist;
    track_id_       = track_id;
    track_explicit_ = is_explicit;
    display_.set_track(title, artist, art_url, is_explicit);
}

// ── GamePad buttons ───────────────────────────────────────────────────────────

void Player::handle_buttons(uint32_t trigger) {
    if (trigger & VPAD_BUTTON_PLUS) {
        volume_ = std::min(100, volume_ + 5);
        audio_->set_volume(volume_);
        display_.set_volume(volume_);
        if (spirc_) spirc_->notify(spirc_playing_, audio_->position_ms(), volume_);
    }
    if (trigger & VPAD_BUTTON_MINUS) {
        volume_ = std::max(0, volume_ - 5);
        audio_->set_volume(volume_);
        display_.set_volume(volume_);
        if (spirc_) spirc_->notify(spirc_playing_, audio_->position_ms(), volume_);
    }
    if ((trigger & VPAD_BUTTON_R) && spirc_) {
        spirc_->skip(true);
    }
    if ((trigger & VPAD_BUTTON_L) && spirc_) {
        spirc_->skip(false);
    }
    if ((trigger & VPAD_BUTTON_ZR) && spirc_ && spirc_playing_) {
        int pos = std::min(audio_->position_ms() + 5000, INT_MAX);
        audio_->seek(pos);
        spirc_->seek_to(pos);
    }
    if ((trigger & VPAD_BUTTON_ZL) && spirc_ && spirc_playing_) {
        int pos = std::max(audio_->position_ms() - 5000, 0);
        audio_->seek(pos);
        spirc_->seek_to(pos);
    }
    if (trigger & VPAD_BUTTON_UP) {
        crystal_enabled_ = true;
        audio_->set_crystalizer(true, crystal_strength_);
        display_.set_crystalizer(true, crystal_strength_);
    }
    if (trigger & VPAD_BUTTON_DOWN) {
        crystal_enabled_ = false;
        audio_->set_crystalizer(false, crystal_strength_);
        display_.set_crystalizer(false, crystal_strength_);
    }
    if (trigger & VPAD_BUTTON_RIGHT) {
        crystal_strength_ = std::min(25, crystal_strength_ + 1);
        audio_->set_crystalizer(crystal_enabled_, crystal_strength_);
        display_.set_crystalizer(crystal_enabled_, crystal_strength_);
    }
    if (trigger & VPAD_BUTTON_LEFT) {
        crystal_strength_ = std::max(1, crystal_strength_ - 1);
        audio_->set_crystalizer(crystal_enabled_, crystal_strength_);
        display_.set_crystalizer(crystal_enabled_, crystal_strength_);
    }
    if (trigger & VPAD_BUTTON_B)
        display_.toggle_controls();
    if ((trigger & VPAD_BUTTON_X) && spirc_) {
        spirc_->toggle_shuffle();
        display_.set_shuffle(spirc_->shuffle());
    }
    if ((trigger & VPAD_BUTTON_Y) && spirc_) {
        spirc_->toggle_repeat();
        display_.set_repeat(spirc_->repeat_mode());
    }
    if ((trigger & VPAD_BUTTON_A) && spirc_) {
        int pos = audio_->position_ms();
        if (spirc_playing_) {
            spirc_playing_ = false;
            audio_->pause();
            display_.set_progress(pos, track_dur_ms_, false);
        } else {
            spirc_playing_ = true;
            audio_->resume();
            display_.set_progress(pos, track_dur_ms_, true);
        }
        spirc_->notify(spirc_playing_, pos, volume_);
    }

    // ── OLV: StickR = toggle post card visibility ────────────────────────────
    if ((trigger & VPAD_BUTTON_STICK_R) && OLV::is_available()) {
        std::lock_guard<std::mutex> lk(olv_mu_);
        olv_card_visible_ = !olv_card_visible_;
        if (olv_card_visible_) {
            if (olv_posts_.empty()) {
                if (!olv_fetch_thread_.joinable())
                    olv_fetch_thread_ = std::thread([this] {
                        OSSetThreadAffinity(OSGetCurrentThread(),
                                            OS_THREAD_ATTRIB_AFFINITY_CPU0);
                        olv_fetch(OLV::COMMUNITY_ID);
                    });
            } else {
                olv_last_advance_ = OSGetSystemTick();
                olv_show_current();
            }
        } else {
            display_.set_olv_post(nullptr);
        }
    }
    if ((trigger & VPAD_BUTTON_STICK_L) && OLV::is_available()) {
        // ♪ Title — Artist  (UTF-8: ♪ = \xe2\x99\xaa, — = \xe2\x80\x94)
        std::string body = "\xe2\x99\xaa " + track_title_ + " \xe2\x80\x94 " + track_artist_;
        OLV::open_post_applet(body, track_explicit_, track_title_ + " - " + track_artist_, track_id_);
    }
}

void Player::handle_wiimote_buttons(uint32_t trigger) {
    if (trigger & WPAD_BUTTON_B)
        display_.toggle_controls();
    if ((trigger & WPAD_BUTTON_PLUS) && spirc_) {
        volume_ = std::min(100, volume_ + 5);
        audio_->set_volume(volume_);
        display_.set_volume(volume_);
        spirc_->notify(spirc_playing_, audio_->position_ms(), volume_);
    }
    if ((trigger & WPAD_BUTTON_MINUS) && spirc_) {
        volume_ = std::max(0, volume_ - 5);
        audio_->set_volume(volume_);
        display_.set_volume(volume_);
        spirc_->notify(spirc_playing_, audio_->position_ms(), volume_);
    }
    if ((trigger & WPAD_BUTTON_RIGHT) && spirc_ && spirc_playing_) {
        int pos = std::min(audio_->position_ms() + 5000, INT_MAX);
        audio_->seek(pos);
        spirc_->seek_to(pos);
    }
    if ((trigger & WPAD_BUTTON_LEFT) && spirc_ && spirc_playing_) {
        int pos = std::max(audio_->position_ms() - 5000, 0);
        audio_->seek(pos);
        spirc_->seek_to(pos);
    }
    if ((trigger & WPAD_BUTTON_UP) && spirc_)
        spirc_->skip(true);
    if ((trigger & WPAD_BUTTON_DOWN) && spirc_)
        spirc_->skip(false);
    if ((trigger & WPAD_BUTTON_A) && spirc_) {
        int pos = audio_->position_ms();
        if (spirc_playing_) {
            spirc_playing_ = false;
            audio_->pause();
            display_.set_progress(pos, track_dur_ms_, false);
        } else {
            spirc_playing_ = true;
            audio_->resume();
            display_.set_progress(pos, track_dur_ms_, true);
        }
        spirc_->notify(spirc_playing_, pos, volume_);
    }
    if ((trigger & WPAD_BUTTON_1) && spirc_) {
        spirc_->toggle_shuffle();
        display_.set_shuffle(spirc_->shuffle());
    }
    if ((trigger & WPAD_BUTTON_2) && spirc_) {
        spirc_->toggle_repeat();
        display_.set_repeat(spirc_->repeat_mode());
    }
}

void Player::handle_pro_buttons(uint32_t trigger) {
    if (trigger & WPAD_PRO_BUTTON_PLUS) {
        volume_ = std::min(100, volume_ + 5);
        audio_->set_volume(volume_);
        display_.set_volume(volume_);
        if (spirc_) spirc_->notify(spirc_playing_, audio_->position_ms(), volume_);
    }
    if (trigger & WPAD_PRO_BUTTON_MINUS) {
        volume_ = std::max(0, volume_ - 5);
        audio_->set_volume(volume_);
        display_.set_volume(volume_);
        if (spirc_) spirc_->notify(spirc_playing_, audio_->position_ms(), volume_);
    }
    if ((trigger & WPAD_PRO_BUTTON_R) && spirc_)
        spirc_->skip(true);
    if ((trigger & WPAD_PRO_BUTTON_L) && spirc_)
        spirc_->skip(false);
    if ((trigger & WPAD_PRO_BUTTON_ZR) && spirc_ && spirc_playing_) {
        int pos = std::min(audio_->position_ms() + 5000, INT_MAX);
        audio_->seek(pos);
        spirc_->seek_to(pos);
    }
    if ((trigger & WPAD_PRO_BUTTON_ZL) && spirc_ && spirc_playing_) {
        int pos = std::max(audio_->position_ms() - 5000, 0);
        audio_->seek(pos);
        spirc_->seek_to(pos);
    }
    if (trigger & WPAD_PRO_BUTTON_UP) {
        crystal_enabled_ = true;
        audio_->set_crystalizer(true, crystal_strength_);
        display_.set_crystalizer(true, crystal_strength_);
    }
    if (trigger & WPAD_PRO_BUTTON_DOWN) {
        crystal_enabled_ = false;
        audio_->set_crystalizer(false, crystal_strength_);
        display_.set_crystalizer(false, crystal_strength_);
    }
    if (trigger & WPAD_PRO_BUTTON_RIGHT) {
        crystal_strength_ = std::min(25, crystal_strength_ + 1);
        audio_->set_crystalizer(crystal_enabled_, crystal_strength_);
        display_.set_crystalizer(crystal_enabled_, crystal_strength_);
    }
    if (trigger & WPAD_PRO_BUTTON_LEFT) {
        crystal_strength_ = std::max(1, crystal_strength_ - 1);
        audio_->set_crystalizer(crystal_enabled_, crystal_strength_);
        display_.set_crystalizer(crystal_enabled_, crystal_strength_);
    }
    if (trigger & WPAD_PRO_BUTTON_B)
        display_.toggle_controls();
    if ((trigger & WPAD_PRO_BUTTON_STICK_R) && OLV::is_available()) {
        std::lock_guard<std::mutex> lk(olv_mu_);
        olv_card_visible_ = !olv_card_visible_;
        if (olv_card_visible_) {
            if (olv_posts_.empty()) {
                if (!olv_fetch_thread_.joinable())
                    olv_fetch_thread_ = std::thread([this] {
                        OSSetThreadAffinity(OSGetCurrentThread(),
                                            OS_THREAD_ATTRIB_AFFINITY_CPU0);
                        olv_fetch(OLV::COMMUNITY_ID);
                    });
            } else {
                olv_last_advance_ = OSGetSystemTick();
                olv_show_current();
            }
        } else {
            display_.set_olv_post(nullptr);
        }
    }
    if ((trigger & WPAD_PRO_BUTTON_STICK_L) && OLV::is_available()) {
        std::string body = "\xe2\x99\xaa " + track_title_ + " \xe2\x80\x94 " + track_artist_;
        OLV::open_post_applet(body, track_explicit_, track_title_ + " - " + track_artist_, track_id_);
    }
    if ((trigger & WPAD_PRO_BUTTON_X) && spirc_) {
        spirc_->toggle_shuffle();
        display_.set_shuffle(spirc_->shuffle());
    }
    if ((trigger & WPAD_PRO_BUTTON_Y) && spirc_) {
        spirc_->toggle_repeat();
        display_.set_repeat(spirc_->repeat_mode());
    }
    if ((trigger & WPAD_PRO_BUTTON_A) && spirc_) {
        int pos = audio_->position_ms();
        if (spirc_playing_) {
            spirc_playing_ = false;
            audio_->pause();
            display_.set_progress(pos, track_dur_ms_, false);
        } else {
            spirc_playing_ = true;
            audio_->resume();
            display_.set_progress(pos, track_dur_ms_, true);
        }
        spirc_->notify(spirc_playing_, pos, volume_);
    }
}

// ── OLV helpers ───────────────────────────────────────────────────────────────

void Player::olv_show_current() {
    // Must be called with olv_mu_ held.
    if (olv_posts_.empty()) {
        display_.set_olv_post(nullptr);
        return;
    }
    const OLV::Post &p = olv_posts_[olv_post_idx_];
    UI::Display::OLVPost dp{ p.body, p.screen_name, p.feeling };
    display_.set_olv_post(&dp);
}

void Player::olv_fetch(uint32_t cid) {
    std::string cur_id = track_id_;  // snapshot before blocking fetch
    auto posts = OLV::fetch_posts(cid, 5, cur_id);

    std::lock_guard<std::mutex> lk(olv_mu_);
    if (!posts.empty()) {
        olv_posts_    = std::move(posts);
        olv_post_idx_ = 0;
        olv_last_advance_ = OSGetSystemTick();
    }
    if (olv_card_visible_)
        olv_show_current();
}

} // namespace Connect
