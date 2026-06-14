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

    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE,
        [](void *) -> uint32_t {
            WHBLogPrint("player: entered background");
            // Signal the cache-sweep WUPS plugin to run a sweep.
            FILE *flag = fopen("/vol/external01/spotify_cache/.sweep_now", "w");
            if (flag) fclose(flag);
            return 0;
        }, nullptr, 100);
    ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE,
        [](void *) -> uint32_t {
            WHBLogPrint("player: returned to foreground");
            return 0;
        }, nullptr, 100);

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

    // CDN fetch failed (not aborted by stop()) — reset to waiting without
    // telling Spotify the track ended, which would cause it to auto-resend
    // the same track and loop indefinitely.
    audio_->on_fetch_error = [this] {
        WHBLogPrint("player: CDN fetch error — resetting to waiting");
        spirc_playing_ = false;
        state_.store(State::WaitingForUser);
        display_.set_waiting();
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
                    } else if (now - olv_last_advance_ >= OSSecondsToTicks(5)) {
                        // ── Time-based fallback ──────────────────────────────────────
                        // 5 s timer already exceeds the 3 s minimum; no extra check needed.
                        olv_last_advance_ = now;
                        olv_post_idx_ = (olv_post_idx_ + 1) % olv_posts_.size();
                        olv_show_current();
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

    // Disconnect AP first — shutdown(fd_) unblocks any recv_exact in the
    // connect thread, causing it to exit quickly.
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

    // Clean up any previous connection (join defunct recv thread, close fd).
    if (spirc_) { spirc_->stop(); spirc_.reset(); }
    reconnecting_.store(true);
    if (ap_) ap_->disconnect();
    reconnecting_.store(false);
    ap_ = std::make_unique<AP>();

    // Stamp this AP session with a generation number.  The on_disconnect closure
    // captures it and returns immediately if it no longer matches — this safely
    // discards late-firing callbacks from the old AP's recv thread without
    // interfering with an already-live new session's audio or display state.
    uint32_t gen = ++ap_gen_;

    AP::Callbacks acb;
    acb.on_packet     = [this](uint8_t cmd, std::vector<uint8_t> pl) {
        on_ap_packet(cmd, std::move(pl));
    };
    acb.on_disconnect = [this, gen] {
        if (ap_gen_.load() != gen) return;  // stale callback from a replaced AP
        WHBLogPrint("player: AP disconnected");
        state_.store(State::WaitingForUser);
        audio_->stop();
        spirc_playing_ = false;
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
    scb.on_became_inactive = [this] {
        audio_->stop();
        spirc_playing_ = false;
        {
            std::lock_guard<std::mutex> lk(olv_mu_);
            olv_posts_.clear();
            olv_post_idx_ = SIZE_MAX;
        }
        display_.set_olv_post(nullptr);
        display_.set_waiting();
    };
    scb.on_track_changed = [this](const std::string &t,
                                  const std::string &a,
                                  const std::string &u,
                                  int64_t dur,
                                  bool expl,
                                  const std::string &id,
                                  const std::string &isrc) {
        on_track_changed(t, a, u, dur, expl, id, isrc);
    };

    spirc_->start(std::move(scb));
    state_.store(State::Ready);
    const std::string &canon = ap_->canonical_username();
    zeroconf_.set_active_user(canon);
    WHBLogPrintf("player: connected as '%s'", canon.c_str());
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
                               const std::string &track_id, const std::string &isrc) {
    track_dur_ms_   = (int)std::min(duration_ms, (int64_t)INT_MAX);
    track_title_    = title;
    track_artist_   = artist;
    track_id_       = track_id;
    isrc_           = isrc;
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

    // ── OLV: StickR = open current post (or community) in Roséverse ──────────
    if ((trigger & VPAD_BUTTON_STICK_R) && OLV::is_available()) {
        std::string post_id;
        bool has_post = false;
        {
            std::lock_guard<std::mutex> lk(olv_mu_);
            if (olv_post_idx_ != SIZE_MAX && olv_post_idx_ < olv_posts_.size()) {
                post_id  = olv_posts_[olv_post_idx_].post_id;
                has_post = true;
            }
        }
        if (has_post) OLV::open_overlay(post_id);
    }
    if ((trigger & VPAD_BUTTON_STICK_L) && OLV::is_available() && spirc_playing_) {
        if (!show_stamp_pack_picker()) return;
        const std::string &post_key = isrc_.empty() ? track_id_ : isrc_;
        OLV::open_post_applet("", track_explicit_, track_title_ + " - " + track_artist_, post_key,
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
    // tpFiltered1 reduces resistive-touch noise; VPAD_TP_1280X720 maps directly to framebuffer space
    VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_1280X720, &tp, &vpad.tpFiltered1);

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

    // Ignore frames with invalid calibration data — garbage coordinates from the
    // resistive panel can otherwise fire the seek zone and set consumed=true,
    // silently swallowing the next art-tap or swipe on release.
    if (tp.validity != VPAD_VALID)
        return;

    const int fx = (int)tp.x;
    const int fy = (int)tp.y;

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
        std::string post_id;
        bool has_post = false;
        {
            std::lock_guard<std::mutex> lk(olv_mu_);
            if (olv_post_idx_ != SIZE_MAX && olv_post_idx_ < olv_posts_.size()) {
                post_id  = olv_posts_[olv_post_idx_].post_id;
                has_post = true;
            }
        }
        if (has_post) OLV::open_overlay(post_id);
    }
    if ((trigger & WPAD_PRO_BUTTON_STICK_L) && OLV::is_available() && spirc_playing_) {
        const std::string &post_key = isrc_.empty() ? track_id_ : isrc_;
        OLV::open_post_applet("", track_explicit_, track_title_ + " - " + track_artist_, post_key,
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
    UI::Display::OLVPost dp{ p.body, p.screen_name, p.feeling, p.memo };
    display_.set_olv_post(&dp);
}

void Player::olv_fetch(uint32_t cid) {
    // Prefer ISRC (region-neutral) over track ID; fall back if Spotify didn't provide one.
    std::string cur_key = isrc_.empty() ? track_id_ : isrc_;
    if (cur_key.empty()) { olv_fetching_ = false; return; }
    auto posts = OLV::fetch_posts(cid, 5, cur_key);

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

// ── Stamp pack picker ─────────────────────────────────────────────────────────

bool Player::show_stamp_pack_picker() {
    SDL_Renderer *ren = display_.renderer();
    TTF_Font *font_md = display_.font_medium();
    TTF_Font *font_sm = display_.font_small();
    if (!ren || !font_md) return true;

    auto all_packs = OLV::fetch_stamp_packs();
    if (all_packs.empty()) return true;

    auto draw_text = [&](TTF_Font *f, const char *txt, SDL_Color col, int x, int y) {
        if (!f || !txt || !*txt) return;
        SDL_Surface *s = TTF_RenderUTF8_Blended(f, txt, col);
        if (!s) return;
        SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
        SDL_Rect r = {x, y, s->w, s->h};
        SDL_RenderCopy(ren, t, nullptr, &r);
        SDL_DestroyTexture(t);
        SDL_FreeSurface(s);
    };

    SDL_Color white = {255, 255, 255, 255};
    SDL_Color green = { 30, 215,  96, 255};
    SDL_Color gray  = {160, 160, 160, 255};

    VPADStatus    vpad{};
    VPADReadError verr{};

    auto show_msg = [&](const char *msg) {
        SDL_SetRenderDrawColor(ren, 10, 10, 20, 255);
        SDL_RenderClear(ren);
        draw_text(font_md, msg, green, 100, 360);
        SDL_RenderPresent(ren);
    };

    auto do_download = [&](const OLV::Pack &pack) -> bool {
        while (WHBProcIsRunning()) {
            show_msg("Downloading stamps...");
            if (OLV::download_stamp_pack(pack) > 0) return true;
            show_msg("Download failed — press A to retry or B to cancel.");
            while (WHBProcIsRunning()) {
                VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);
                if (vpad.trigger & VPAD_BUTTON_B) return false;
                if (vpad.trigger & VPAD_BUTTON_A) break;
                OSSleepTicks(OSMillisecondsToTicks(16));
            }
        }
        return false;
    };

    // Renders a titled list of packs with a highlight bar on the selected row.
    auto render_screen = [&](const char *title, const std::vector<OLV::Pack> &items,
                             int sel, const char *hints) {
        SDL_SetRenderDrawColor(ren, 10, 10, 20, 255);
        SDL_RenderClear(ren);
        draw_text(font_md, title, white, 100, 60);
        if (items.empty())
            draw_text(font_sm, "Nothing here yet.", gray, 100, 200);
        for (int i = 0; i < (int)items.size(); ++i) {
            int y = 150 + i * 80;
            bool is_sel = (i == sel);
            if (is_sel) {
                SDL_SetRenderDrawColor(ren, 35, 35, 50, 255);
                SDL_Rect row = {80, y - 8, 1100, 64};
                SDL_RenderFillRect(ren, &row);
                SDL_SetRenderDrawColor(ren, 30, 215, 96, 255);
                SDL_Rect bar = {80, y - 8, 4, 64};
                SDL_RenderFillRect(ren, &bar);
            }
            draw_text(font_md, items[i].name.c_str(), is_sel ? green : white, 100, y);
            if (!items[i].description.empty())
                draw_text(font_sm, items[i].description.c_str(), gray, 100, y + 32);
            if (items[i].id != "none") {
                int n = OLV::cached_stamp_count(items[i].id);
                char buf[48] = {};
                if (n > 0) snprintf(buf, sizeof(buf), "%d stamp%s", n, n == 1 ? "" : "s");
                else        snprintf(buf, sizeof(buf), "not installed");
                draw_text(font_sm, buf, gray, 900, y + 20);
            }
        }
        draw_text(font_sm, hints, gray, 100, 660);
        SDL_RenderPresent(ren);
    };

    // ── Stamp Store: browse and download packs not yet on SD ─────────────────
    auto run_store = [&]() {
        int sel = 0;
        while (WHBProcIsRunning()) {
            std::vector<OLV::Pack> available;
            for (const auto &p : all_packs)
                if (OLV::cached_stamp_count(p.id) == 0) available.push_back(p);
            sel = std::min(sel, std::max(0, (int)available.size() - 1));
            render_screen("Stamp Store", available, sel,
                          "Up/Down: Navigate     A: Download     B: Back");
            VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);
            if (vpad.trigger & VPAD_BUTTON_B) return;
            if (vpad.trigger & VPAD_BUTTON_UP)
                sel = (sel - 1 + std::max(1, (int)available.size())) % std::max(1, (int)available.size());
            if (vpad.trigger & VPAD_BUTTON_DOWN)
                sel = (sel + 1) % std::max(1, (int)available.size());
            if ((vpad.trigger & VPAD_BUTTON_A) && !available.empty()) {
                if (do_download(available[sel])) {
                    show_msg("Downloaded! Press B to return.");
                    while (WHBProcIsRunning()) {
                        VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);
                        if (vpad.trigger & VPAD_BUTTON_B) return;
                        OSSleepTicks(OSMillisecondsToTicks(16));
                    }
                }
            }
            OSSleepTicks(OSMillisecondsToTicks(16));
        }
    };

    // ── Pack Manager: update or delete installed packs ────────────────────────
    auto run_manager = [&]() {
        int sel = 0;
        while (WHBProcIsRunning()) {
            std::vector<OLV::Pack> installed;
            for (const auto &p : all_packs)
                if (OLV::cached_stamp_count(p.id) > 0) installed.push_back(p);
            sel = std::min(sel, std::max(0, (int)installed.size() - 1));
            render_screen("Pack Manager", installed, sel,
                          "Up/Down: Navigate     Y: Update     X: Delete     B: Back");
            VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);
            if (vpad.trigger & VPAD_BUTTON_B) return;
            if (vpad.trigger & VPAD_BUTTON_UP)
                sel = (sel - 1 + std::max(1, (int)installed.size())) % std::max(1, (int)installed.size());
            if (vpad.trigger & VPAD_BUTTON_DOWN)
                sel = (sel + 1) % std::max(1, (int)installed.size());
            if (!installed.empty()) {
                if (vpad.trigger & VPAD_BUTTON_Y) {
                    if (do_download(installed[sel])) {
                        show_msg("Updated.");
                        OSSleepTicks(OSMillisecondsToTicks(800));
                    }
                }
                if (vpad.trigger & VPAD_BUTTON_X) {
                    const std::string &pid = installed[sel].id;
                    OLV::delete_stamp_pack(pid);
                    if (OLV::load_selected_pack() == pid) {
                        OLV::save_selected_pack("none");
                        OLV::load_stamp_pack("none");
                    }
                    show_msg("Pack deleted.");
                    OSSleepTicks(OSMillisecondsToTicks(800));
                    sel = 0;
                }
            }
            OSSleepTicks(OSMillisecondsToTicks(16));
        }
    };

    // ── Main Picker: No Stamps + installed packs ──────────────────────────────
    OLV::Pack none_pack;
    none_pack.id = "none"; none_pack.name = "No Stamps";
    none_pack.description = "Post without stamps";

    // Restore cursor to previously selected pack.
    int sel = 0;
    {
        std::string current = OLV::load_selected_pack();
        if (current != "none") {
            int idx = 1;
            for (const auto &p : all_packs) {
                if (p.id == current && OLV::cached_stamp_count(p.id) > 0) { sel = idx; break; }
                ++idx;
            }
        }
    }

    while (WHBProcIsRunning()) {
        std::vector<OLV::Pack> installed;
        installed.push_back(none_pack);
        for (const auto &p : all_packs)
            if (OLV::cached_stamp_count(p.id) > 0) installed.push_back(p);
        sel = std::min(sel, (int)installed.size() - 1);

        render_screen("Select Stamp Pack", installed, sel,
                      "Up/Down: Navigate     A: Use     +: Store     -: Manage     B: Cancel");
        VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);
        if (vpad.trigger & VPAD_BUTTON_B) return false;
        if (vpad.trigger & VPAD_BUTTON_UP)
            sel = (sel - 1 + (int)installed.size()) % (int)installed.size();
        if (vpad.trigger & VPAD_BUTTON_DOWN)
            sel = (sel + 1) % (int)installed.size();
        if (vpad.trigger & VPAD_BUTTON_A) {
            OLV::load_stamp_pack(installed[sel].id);
            OLV::save_selected_pack(installed[sel].id);
            return true;
        }
        if (vpad.trigger & VPAD_BUTTON_PLUS)  run_store();
        if (vpad.trigger & VPAD_BUTTON_MINUS) run_manager();
        OSSleepTicks(OSMillisecondsToTicks(16));
    }
    return false;
}

} // namespace Connect
