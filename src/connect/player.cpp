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
    }, [this](std::string msg) {
        display_.set_error(std::move(msg));
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
        if (verr == VPAD_READ_SUCCESS) {
            handle_buttons(vpad.trigger);
            handle_touch(&vpad);
        }

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

        // OLV card auto-advance while visible.
        // Two modes depending on whether fetched posts carry timestamp metadata:
        //   Timestamp mode: advance to the post whose position_ms best matches the
        //     current track position; refresh the post list every 30 s.
        //   Time-based fallback (no metadata): rotate every 5 s, refetch on wrap.
        // Both modes enforce a 3 s minimum dwell before any advance.
        if (OLV::is_available()) {
            uint64_t now = OSGetSystemTick();
            std::lock_guard<std::mutex> lk(olv_mu_);
            if (!olv_fetching_) {
                if (!olv_posts_.empty()) {
                    bool dwell_ok = (now - olv_shown_at_ >= OSSecondsToTicks(3));
                    if (olv_posts_[0].position_ms > 0) {
                        // ── Timestamp mode ──────────────────────────────────────────
                        if (dwell_ok) {
                            uint32_t cur_pos = (uint32_t)std::max(0, audio_->position_ms());
                            // Rightmost post whose timestamp has been reached.
                            bool found = false;
                            size_t target = 0;
                            for (size_t i = 0; i < olv_posts_.size(); ++i) {
                                if (olv_posts_[i].position_ms <= cur_pos) {
                                    target = i;
                                    found = true;
                                }
                            }
                            if (found) {
                                if (target != olv_post_idx_) {
                                    olv_post_idx_ = target;
                                    olv_show_current();
                                }
                            } else if (olv_post_idx_ != SIZE_MAX) {
                                // User seeked back before the first post's timestamp.
                                olv_post_idx_ = SIZE_MAX;
                                display_.set_olv_post(nullptr);
                            }
                        }
                        // Refresh post list every 30 s.
                        if (now - olv_last_advance_ >= OSSecondsToTicks(30)) {
                            if (olv_fetch_thread_.joinable()) olv_fetch_thread_.join();
                            olv_fetching_ = true;
                            olv_fetch_thread_ = std::thread([this] {
                                OSSetThreadAffinity(OSGetCurrentThread(),
                                                    OS_THREAD_ATTRIB_AFFINITY_CPU0);
                                olv_fetch(OLV::COMMUNITY_ID);
                            });
                        }
                    } else if (now - olv_last_advance_ >= OSSecondsToTicks(5)) {
                        // ── Time-based fallback ──────────────────────────────────────
                        // 5 s timer already exceeds the 3 s minimum; no extra check needed.
                        olv_last_advance_ = now;
                        olv_post_idx_ = (olv_post_idx_ + 1) % olv_posts_.size();
                        if (olv_post_idx_ == 0) {
                            if (olv_fetch_thread_.joinable()) olv_fetch_thread_.join();
                            olv_fetching_ = true;
                            olv_fetch_thread_ = std::thread([this] {
                                OSSetThreadAffinity(OSGetCurrentThread(),
                                                    OS_THREAD_ATTRIB_AFFINITY_CPU0);
                                olv_fetch(OLV::COMMUNITY_ID);
                            });
                        } else {
                            olv_show_current();
                        }
                    }
                } else if (now - olv_last_advance_ >= OSSecondsToTicks(5)) {
                    // No posts yet — kick off initial fetch.
                    olv_last_advance_ = now;
                    if (olv_fetch_thread_.joinable()) olv_fetch_thread_.join();
                    olv_fetching_ = true;
                    olv_fetch_thread_ = std::thread([this] {
                        OSSetThreadAffinity(OSGetCurrentThread(),
                                            OS_THREAD_ATTRIB_AFFINITY_CPU0);
                        olv_fetch(OLV::COMMUNITY_ID);
                    });
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
    {
        std::lock_guard<std::mutex> lk(olv_mu_);
        olv_posts_.clear();
        olv_post_idx_ = SIZE_MAX;
        olv_last_advance_ = 0;  // trigger immediate fetch on next main loop tick
    }
    display_.set_olv_post(nullptr);  // clear card immediately; refetch will repopulate it
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

    // ── OLV: StickR = open current post in Roséverse overlay ────────────────
    if ((trigger & VPAD_BUTTON_STICK_R) && OLV::is_available()) {
        OLV::open_overlay();
    }
    if ((trigger & VPAD_BUTTON_STICK_L) && OLV::is_available()) {
        // ♪ Title — Artist  (UTF-8: ♪ = \xe2\x99\xaa, — = \xe2\x80\x94)
        std::string body = "\xe2\x99\xaa " + track_title_ + " \xe2\x80\x94 " + track_artist_;
        OLV::open_post_applet(body, track_explicit_, track_title_ + " - " + track_artist_, track_id_,
                              (uint32_t)std::max(0, audio_->position_ms()), (uint32_t)track_dur_ms_);
    }
}

// ── GamePad touch ─────────────────────────────────────────────────────────────
//
// VPADGetTPCalibratedPoint returns physical DRC coordinates (0–853, 0–479).
// The framebuffer is 1280×720, so we scale up: fx = tx*1280/854, fy = ty*720/480.
// After scaling, coordinates are directly comparable to Theme:: constants.
//
// Touch zones (1280×720 framebuffer space, see theme.h):
//   Progress bar seek : x 40–1240,  y 600–660  (expanded tap target around BAR_Y=620)
//   Album art tap     : x 40–400,   y 60–420   → play/pause
//   Swipe anywhere    : |dx| > 120, |dy| < 90  → next (left) / prev (right)
//
// Seek fires continuously while held; swipe and art-tap fire once on release.

void Player::handle_touch(const void *vpad_status_ptr) {
    const VPADStatus &vpad = *static_cast<const VPADStatus *>(vpad_status_ptr);

    VPADTouchData tp{};
    VPADGetTPCalibratedPoint(VPAD_CHAN_0, &tp, &vpad.tpNormal);

    // Scale from DRC physical (854×480) to framebuffer space (1280×720)
    const int fx = (int)tp.x * 1280 / 854;
    const int fy = (int)tp.y * 720  / 480;

    if (!tp.touched) {
        if (touch_.active && !touch_.consumed && spirc_) {
            int dx = (int)touch_.cur_x - touch_.start_x;
            int dy = (int)touch_.cur_y - touch_.start_y;

            // Horizontal swipe → next / prev track
            if (std::abs(dx) >= 120 && std::abs(dy) < 90) {
                if (dx < 0)
                    spirc_->skip(true);   // swipe left → next
                else
                    spirc_->skip(false);  // swipe right → prev
            }
            // Tap on album art → play/pause
            else if (touch_.start_x >= 40  && touch_.start_x <= 400 &&
                     touch_.start_y >= 60  && touch_.start_y <= 420 &&
                     std::abs(dx) < 30 && std::abs(dy) < 30) {
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
        touch_ = {};
        return;
    }

    if (!touch_.active) {
        touch_.active   = true;
        touch_.consumed = false;
        touch_.start_x  = (int16_t)fx;
        touch_.start_y  = (int16_t)fy;
    }
    touch_.cur_x = (int16_t)fx;
    touch_.cur_y = (int16_t)fy;

    // Progress bar: seek continuously while finger is held on bar
    if (!touch_.consumed &&
        fy >= 600 && fy <= 660 &&
        fx >= 40  && fx <= 1240 &&
        spirc_ && track_dur_ms_ > 0) {
        float frac   = float(fx - 40) / 1200.0f;
        int   pos_ms = int(frac * track_dur_ms_);
        audio_->seek(pos_ms);
        spirc_->seek_to(pos_ms);
        touch_.consumed = true;
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
        OLV::open_overlay();
    }
    if ((trigger & WPAD_PRO_BUTTON_STICK_L) && OLV::is_available()) {
        std::string body = "\xe2\x99\xaa " + track_title_ + " \xe2\x80\x94 " + track_artist_;
        OLV::open_post_applet(body, track_explicit_, track_title_ + " - " + track_artist_, track_id_,
                              (uint32_t)std::max(0, audio_->position_ms()), (uint32_t)track_dur_ms_);
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
    // olv_post_idx_ == SIZE_MAX means "timestamp mode, no post due yet".
    if (olv_posts_.empty() || olv_post_idx_ >= olv_posts_.size()) {
        display_.set_olv_post(nullptr);
        return;
    }
    olv_shown_at_ = OSGetSystemTick();
    const OLV::Post &p = olv_posts_[olv_post_idx_];
    UI::Display::OLVPost dp{ p.body, p.screen_name, p.feeling };
    display_.set_olv_post(&dp);
}

void Player::olv_fetch(uint32_t cid) {
    std::string cur_id = track_id_;  // snapshot before blocking fetch
    auto posts = OLV::fetch_posts(cid, 5, cur_id);

    {
        std::lock_guard<std::mutex> lk(olv_mu_);
        if (!posts.empty()) {
            // Stable sort by position_ms asc. Server returns posts latest-first, so
            // ties (same timestamp) keep the latest post first — satisfying "prefer
            // latest on time conflicts" without extra tie-breaking logic.
            std::stable_sort(posts.begin(), posts.end(),
                [](const OLV::Post &a, const OLV::Post &b) {
                    return a.position_ms < b.position_ms;
                });
            olv_posts_    = std::move(posts);
            // Timestamp mode: start with SIZE_MAX ("nothing due yet") so the
            // advance loop shows the first post only when its timestamp is reached.
            // Time-based fallback: start at 0 as usual.
            olv_post_idx_ = (olv_posts_[0].position_ms > 0) ? SIZE_MAX : 0;
            olv_last_advance_ = OSGetSystemTick();
        }
        olv_show_current();
    }
    olv_fetching_ = false;
}

} // namespace Connect
