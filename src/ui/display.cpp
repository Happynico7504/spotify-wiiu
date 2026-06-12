#include "display.h"
#include "theme.h"
#include "font_data.h"
#include "../connect/audio.h"
#include "../olv/olv.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_THREAD_LOCALS   // Wii U has no TLS support
#include "stb_image.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

#include <curl/curl.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <whb/log.h>

namespace UI {

// ── Constructor / destructor ──────────────────────────────────────────────────

Display::Display()  = default;
Display::~Display() { shutdown(); }

// ── init / shutdown ───────────────────────────────────────────────────────────

bool Display::init(const char *font_path) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        WHBLogPrintf("display: SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    if (TTF_Init() < 0) {
        WHBLogPrintf("display: TTF_Init failed: %s", TTF_GetError());
        return false;
    }

    tv_win_ = SDL_CreateWindow("Spotify Wii U",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                1280, 720, SDL_WINDOW_SHOWN);
    if (!tv_win_) { WHBLogPrintf("display: TV window: %s", SDL_GetError()); return false; }

    tv_ren_ = SDL_CreateRenderer(tv_win_, -1,
                                  SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!tv_ren_) { WHBLogPrintf("display: TV renderer: %s", SDL_GetError()); return false; }

    // Try file path first; fall back to the font embedded in the binary.
    auto open_font = [&](int pt) -> TTF_Font * {
        TTF_Font *f = font_path ? TTF_OpenFont(font_path, pt) : nullptr;
        if (!f) {
            SDL_RWops *rw = SDL_RWFromConstMem(kFontData, (int)kFontSize);
            if (rw) { f = TTF_OpenFontRW(rw, 1 /*freesrc*/, pt); }
        }
        return f;
    };
    font_title_ = open_font(51);
    font_lg_    = open_font(33);
    font_md_    = open_font(24);
    font_sm_    = open_font(18);

    if (!font_title_ || !font_lg_)
        WHBLogPrintf("display: font load failed: %s", TTF_GetError());

    // Pre-bake the background gradient into a 2×720 static texture.
    // SDL_RenderCopy will stretch it to fill the screen each frame,
    // replacing 720 per-frame SDL_RenderDrawLine + SDL_SetRenderDrawColor calls.
    // A 2-pixel-wide surface is used so both pixels per row are identical;
    // horizontal stretching is therefore colour-correct regardless of filter mode.
    {
        const SDL_Color &t = Theme::BG_TOP, &b = Theme::BG;
        int dr = (int)b.r - (int)t.r, dg = (int)b.g - (int)t.g, db = (int)b.b - (int)t.b;
        constexpr int GH = 720;
        // Use SDL_MapRGBA so pixel packing is endian-safe.
        SDL_Surface *s = SDL_CreateRGBSurface(0, 2, GH, 32,
            0xFF000000u, 0x00FF0000u, 0x0000FF00u, 0x000000FFu);
        if (s) {
            for (int y = 0; y < GH; ++y) {
                uint32_t px = SDL_MapRGBA(s->format,
                    (uint8_t)((int)t.r + dr * y / GH),
                    (uint8_t)((int)t.g + dg * y / GH),
                    (uint8_t)((int)t.b + db * y / GH), 255);
                uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + y * s->pitch);
                row[0] = row[1] = px;
            }
            bg_tex_ = SDL_CreateTextureFromSurface(tv_ren_, s);
            SDL_FreeSurface(s);
        }
    }

    // Pre-bake the static controls hint label.
    if (font_sm_)
        update_label(lc_bhint_, tv_ren_, font_sm_, "B: Controls", Theme::TEXT_HINT);

    art_stop_.store(false);
    art_thread_ = std::thread(&Display::art_worker, this);

    spec_stop_.store(false);
    spec_thread_ = std::thread(&Display::spec_worker, this);

    WHBLogPrint("display: OK");
    return true;
}

void Display::shutdown() {
    spec_stop_.store(true);
    if (spec_thread_.joinable()) spec_thread_.join();

    art_stop_.store(true);
    if (art_thread_.joinable()) art_thread_.join();

    lc_title_.free(); lc_artist_.free();
    lc_pos_.free();   lc_dur_.free();   lc_vol_.free();
    lc_shuf_.free();  lc_rep_.free();   lc_xtal_.free();  lc_bhint_.free();
    lc_wait_[0].free(); lc_wait_[1].free();
    lc_olv_header_.free(); lc_olv_body_.free();
    if (bg_tex_)       { SDL_DestroyTexture(bg_tex_);       bg_tex_       = nullptr; }
    if (art_tex_)      { SDL_DestroyTexture(art_tex_);      art_tex_      = nullptr; }
    if (olv_memo_tex_) { SDL_DestroyTexture(olv_memo_tex_); olv_memo_tex_ = nullptr; }
    if (font_sm_)    TTF_CloseFont(font_sm_);
    if (font_md_)    TTF_CloseFont(font_md_);
    if (font_lg_)    TTF_CloseFont(font_lg_);
    if (font_title_) TTF_CloseFont(font_title_);
    if (tv_ren_)     SDL_DestroyRenderer(tv_ren_);
    if (tv_win_)     SDL_DestroyWindow(tv_win_);
    font_sm_ = font_md_ = font_lg_ = font_title_ = nullptr;
    tv_ren_ = nullptr; tv_win_ = nullptr;
    TTF_Quit();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

// ── State setters ─────────────────────────────────────────────────────────────

void Display::set_waiting() {
    std::lock_guard<std::mutex> lk(mu_);
    waiting_ = true;
    error_msg_.clear();
    title_ = artist_ = art_url_ = "";
    pos_ms_ = dur_ms_ = 0; playing_ = false;
}

void Display::set_error(const std::string &msg) {
    std::lock_guard<std::mutex> lk(mu_);
    waiting_ = true;
    error_msg_ = msg;
    title_ = artist_ = art_url_ = "";
    pos_ms_ = dur_ms_ = 0; playing_ = false;
}

void Display::set_track(const std::string &title, const std::string &artist,
                         const std::string &album_art_url, bool is_explicit) {
    std::lock_guard<std::mutex> lk(mu_);
    waiting_  = false;
    title_    = title;
    artist_   = artist;
    art_url_  = album_art_url;
    explicit_ = is_explicit;
}

void Display::set_progress(int pos_ms, int dur_ms, bool playing) {
    std::lock_guard<std::mutex> lk(mu_);
    pos_ms_ = pos_ms; dur_ms_ = dur_ms; playing_ = playing;
}

void Display::set_volume(int pct) {
    std::lock_guard<std::mutex> lk(mu_); volume_ = pct;
}

void Display::set_shuffle(bool on) {
    std::lock_guard<std::mutex> lk(mu_); shuffle_ = on;
}

void Display::set_repeat(int mode) {
    std::lock_guard<std::mutex> lk(mu_); repeat_mode_ = mode;
}

void Display::set_crystalizer(bool on, int strength) {
    std::lock_guard<std::mutex> lk(mu_);
    crystal_enabled_  = on;
    crystal_strength_ = strength;
}

void Display::toggle_controls() {
    std::lock_guard<std::mutex> lk(mu_); controls_ = !controls_;
}

void Display::set_olv_post(const OLVPost *post) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!post) {
        olv_visible_       = false;
        olv_body_          = "";
        olv_header_        = "";
        olv_memo_pending_.clear();
        olv_memo_dirty_    = true;
        return;
    }
    olv_visible_ = true;
    olv_body_    = post->body;

    // Build header line:  @ScreenName  :)
    const char *feeling = (post->feeling >= 0 && post->feeling <= 5)
                        ? OLV::FEELING_STR[post->feeling] : "";
    olv_header_ = "@" + post->screen_name;
    if (feeling && feeling[0]) { olv_header_ += "  "; olv_header_ += feeling; }

    olv_memo_pending_ = post->memo;
    olv_memo_dirty_   = true;
}

// ── render (main thread) ──────────────────────────────────────────────────────

void Display::render() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {}  // drain events to keep window manager happy

    // Upload pending album art texture (GPU ops must be on main thread)
    {
        std::lock_guard<std::mutex> lk(art_mu_);
        if (art_ready_ && tv_ren_ && !art_pending_data_.empty()) {
            if (art_tex_) { SDL_DestroyTexture(art_tex_); art_tex_ = nullptr; }
            SDL_RWops *rw = SDL_RWFromMem(art_pending_data_.data(),
                                           (int)art_pending_data_.size());
            if (rw) SDL_RWclose(rw);
            int iw, ih, ch;
            unsigned char *px = stbi_load_from_memory(
                art_pending_data_.data(), (int)art_pending_data_.size(),
                &iw, &ih, &ch, 4);
            if (px) {
                SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
                    px, iw, ih, 32, iw * 4, SDL_PIXELFORMAT_RGBA32);
                if (surf) {
                    art_tex_ = SDL_CreateTextureFromSurface(tv_ren_, surf);
                    SDL_FreeSurface(surf);
                    art_loaded_url_ = art_pending_url_;
                    WHBLogPrintf("display: art loaded %dx%d", iw, ih);
                }
                stbi_image_free(px);
            } else {
                WHBLogPrintf("display: art decode failed: %s", stbi_failure_reason());
            }
            art_pending_data_.clear();
            art_ready_ = false;
        }
    }

    // Upload pending OLV drawing texture (GPU ops must be on main thread)
    {
        bool dirty = false;
        std::vector<uint8_t> memo_data;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (olv_memo_dirty_) {
                olv_memo_dirty_ = false;
                dirty = true;
                memo_data = std::move(olv_memo_pending_);
            }
        }
        if (dirty && tv_ren_) {
            if (olv_memo_tex_) { SDL_DestroyTexture(olv_memo_tex_); olv_memo_tex_ = nullptr; }
            if (!memo_data.empty()) {
                // TGA: 18-byte header, then 320×120 pixels in BGRA order from bottom-left row.
                // Flip vertically and swap B/R channels to get top-left RGBA for SDL.
                constexpr int MW = 320, MH = 120, HDR = 18;
                if (memo_data.size() >= (size_t)(HDR + MW * MH * 4)) {
                    const uint8_t *src = memo_data.data() + HDR;
                    std::vector<uint8_t> rgba(MW * MH * 4);
                    for (int row = 0; row < MH; ++row) {
                        const uint8_t *s = src + (MH - 1 - row) * MW * 4;
                        uint8_t       *d = rgba.data() + row * MW * 4;
                        for (int x = 0; x < MW; ++x, s += 4, d += 4) {
                            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                        }
                    }
                    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
                        rgba.data(), MW, MH, 32, MW * 4, SDL_PIXELFORMAT_RGBA32);
                    if (surf) {
                        olv_memo_tex_ = SDL_CreateTextureFromSurface(tv_ren_, surf);
                        SDL_FreeSurface(surf);
                        WHBLogPrint("display: olv drawing loaded");
                    }
                }
            }
        }
    }

    if (tv_ren_)  { render_to(tv_ren_,  1280, 720); SDL_RenderPresent(tv_ren_); }
}

void Display::set_audio(Connect::AudioPipeline *audio) { audio_src_ = audio; }

// ── Spectrum helpers ──────────────────────────────────────────────────────────

static void fft_inplace(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -(float)M_PI * 2.0f / (float)len;
        float ca = cosf(ang), sa = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int j = 0; j < len / 2; ++j) {
                float ur = re[i+j],              ui = im[i+j];
                float vr = re[i+j+len/2]*cr - im[i+j+len/2]*ci;
                float vi = re[i+j+len/2]*ci + im[i+j+len/2]*cr;
                re[i+j]         = ur + vr;  im[i+j]         = ui + vi;
                re[i+j+len/2]   = ur - vr;  im[i+j+len/2]   = ui - vi;
                float nr = cr*ca - ci*sa; ci = cr*sa + ci*ca; cr = nr;
            }
        }
    }
}

static SDL_Color hsv_to_sdl(float h, float s, float v) {
    float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    float r, g, b;
    if      (h < 60)  { r=c; g=x; b=0; }
    else if (h < 120) { r=x; g=c; b=0; }
    else if (h < 180) { r=0; g=c; b=x; }
    else if (h < 240) { r=0; g=x; b=c; }
    else if (h < 300) { r=x; g=0; b=c; }
    else              { r=c; g=0; b=x; }
    return { (uint8_t)((r+m)*255), (uint8_t)((g+m)*255), (uint8_t)((b+m)*255), 255 };
}

// ── Text label cache helpers ──────────────────────────────────────────────────

bool Display::update_label(CachedLabel &lbl, SDL_Renderer *r, TTF_Font *f,
                            const char *text, SDL_Color col, int max_w) {
    bool same = lbl.tex && lbl.str == text && lbl.max_w == max_w &&
                lbl.col.r == col.r && lbl.col.g == col.g && lbl.col.b == col.b;
    if (same) return false;

    lbl.free();
    lbl.str   = text;
    lbl.col   = col;
    lbl.max_w = max_w;

    std::string t = text;
    if (max_w >= 0) {
        int tw, th;
        TTF_SizeUTF8(f, t.c_str(), &tw, &th);
        if (tw > max_w) {
            while (t.size() > 1) {
                t.pop_back();
                std::string cand = t + "\xe2\x80\xa6";
                TTF_SizeUTF8(f, cand.c_str(), &tw, &th);
                if (tw <= max_w) { t = std::move(cand); break; }
            }
        }
    }

    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, t.c_str(), col);
    if (!surf) return false;
    lbl.w = surf->w; lbl.h = surf->h;
    lbl.tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_FreeSurface(surf);
    return true;
}

void Display::draw_label(SDL_Renderer *r, const CachedLabel &lbl, int x, int y, int cx) {
    if (!lbl.tex) return;
    SDL_Rect dst = { (cx >= 0 ? cx - lbl.w / 2 : x), y, lbl.w, lbl.h };
    SDL_RenderCopy(r, lbl.tex, nullptr, &dst);
}

// ── render_to ─────────────────────────────────────────────────────────────────
// Snapshots state under mu_, then renders while NOT holding the lock.

void Display::render_to(SDL_Renderer *r, int w, int h) {
    bool snap_waiting, snap_controls, snap_olv;
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_waiting  = waiting_;
        snap_controls = controls_;
        snap_olv      = olv_visible_;
        snap_error_   = error_msg_;
    }

    if (bg_tex_) {
        SDL_RenderCopy(r, bg_tex_, nullptr, nullptr);
    } else {
        SDL_SetRenderDrawColor(r, Theme::BG.r, Theme::BG.g, Theme::BG.b, 255);
        SDL_RenderClear(r);
    }

    if      (snap_controls) render_controls(r, w, h);
    else if (snap_waiting)  render_waiting(r, w, h);
    else                    render_playing(r, w, h);

    // OLV card overlays the bottom strip when visible (independent of controls/waiting)
    if (snap_olv && !snap_controls) render_olv_card(r, w, h);
}

void Display::render_waiting(SDL_Renderer *r, int w, int h) {
    if (snap_error_.empty()) {
        update_label(lc_wait_[0], r, font_lg_,
                     "Waiting for Spotify\xe2\x80\xa6", Theme::TEXT_DIM);
        update_label(lc_wait_[1], r, font_md_,
                     "Open Spotify and select this device", Theme::TEXT_HINT);
    } else {
        constexpr SDL_Color WARN = {220, 80, 40, 255};
        update_label(lc_wait_[0], r, font_lg_, snap_error_.c_str(), WARN);
        update_label(lc_wait_[1], r, font_md_,
                     "spotify-wiiu.nicochristmann.net", Theme::TEXT_HINT);
    }
    draw_label(r, lc_wait_[0], 0, h / 2 - 14, w / 2);
    draw_label(r, lc_wait_[1], 0, h / 2 + 16, w / 2);
}

void Display::render_controls(SDL_Renderer *r, int w, int h) {
    float s = w / 1280.0f;
    auto S = [&](int v) { return (int)(v * s); };

    // Semi-transparent dark overlay
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
    SDL_Rect full = {0, 0, w, h};
    SDL_RenderFillRect(r, &full);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    draw_centered(r, font_title_, "Controls", Theme::TEXT, w / 2, S(40));

    struct Row { const char *btn; const char *action; };
    static constexpr Row gamepad[] = {
        { "GamePad / Pro Con", ""                       },
        { "A",           "Play / Pause"                },
        { "R / L",       "Skip Fwd / Back"             },
        { "ZR / ZL",     "Seek +5s / -5s"             },
        { "+ / -",       "Volume Up / Down"            },
        { "X",           "Shuffle"                     },
        { "Y",           "Repeat"                      },
        { "Up / Down",   "Crystalizer On / Off"        },
        { "Left / Right","Crystalizer Strength"        },
        { "Stick R",     "Open post in Roséverse"       },
        { "Stick L",     "Post to Roséverse"           },
        { "B",           "This screen"                 },
    };
    static constexpr Row wiimote[] = {
        { "Wiimote",  ""                  },
        { "A",        "Play / Pause"      },
        { "Up / Down","Skip Fwd / Back"   },
        { "Left/Right","Seek +5s / -5s"  },
        { "+ / -",    "Volume Up / Down"  },
        { "1",        "Shuffle"           },
        { "2",        "Repeat"            },
        { "B",        "This screen"       },
    };

    int col_x[2] = { S(160), S(740) };
    int row_h    = S(44);
    int top_y    = S(130);

    for (int col = 0; col < 2; ++col) {
        const Row *rows = (col == 0) ? gamepad : wiimote;
        int nrows = (col == 0) ? 12 : 8;
        int x = col_x[col];
        for (int i = 0; i < nrows; ++i) {
            int y = top_y + i * row_h;
            if (rows[i].action[0] == '\0') {
                // Section header
                draw_clipped(r, font_lg_, rows[i].btn, Theme::ACCENT, x, y, S(500));
            } else {
                draw_clipped(r, font_md_, rows[i].btn,    Theme::TEXT,     x,          y, S(220));
                draw_clipped(r, font_md_, rows[i].action, Theme::TEXT_DIM, x + S(230), y, S(260));
            }
        }
    }

    // Cache size footer — populated after first GC sweep (~startup).
    if (audio_src_) {
        int64_t cb = audio_src_->cache_total_bytes();
        if (cb >= 0) {
            char cache_str[48];
            if      (cb >= (int64_t)1024*1024*1024)
                snprintf(cache_str, sizeof(cache_str), "Cache: %.1f GB", cb / (1024.0*1024*1024));
            else if (cb >= 1024*1024)
                snprintf(cache_str, sizeof(cache_str), "Cache: %.0f MB", cb / (1024.0*1024));
            else
                snprintf(cache_str, sizeof(cache_str), "Cache: %lld KB", (long long)(cb / 1024));
            draw_centered(r, font_sm_, cache_str, Theme::TEXT_HINT, w / 2, h - S(70));
        }
    }
    draw_centered(r, font_sm_, "Press B to close", Theme::TEXT_HINT, w / 2, h - S(40));
}

void Display::render_playing(SDL_Renderer *r, int w, int h) {
    // Snapshot all needed display state
    std::string title, artist;
    int pos_ms, dur_ms, volume;
    bool playing, shuffle, xtal_on, is_explicit;
    int  repeat_mode, xtal_str;
    SDL_Texture *art = art_tex_;  // main-thread only; no lock needed

    {
        std::lock_guard<std::mutex> lk(mu_);
        title    = title_;
        artist   = artist_;
        pos_ms   = pos_ms_;
        dur_ms   = dur_ms_;
        playing  = playing_;
        volume   = volume_;
        shuffle     = shuffle_;
        repeat_mode = repeat_mode_;
        xtal_on  = crystal_enabled_;
        xtal_str = crystal_strength_;
        is_explicit = explicit_;
    }

    float s = w / 1280.0f;  // scale factor
    auto S  = [&](int v) { return (int)(v * s); };

    // ── Album art ─────────────────────────────────────────────────────────────
    int ax = S(Theme::ART_X), ay = S(Theme::ART_Y), asz = S(Theme::ART_SIZE);
    if (art) {
        SDL_Rect dst = {ax, ay, asz, asz};
        SDL_RenderCopy(r, art, nullptr, &dst);
    } else {
        fill_rounded(r, ax, ay, asz, asz, S(10), Theme::ART_FILL);
        if (!artist.empty()) {
            char ch[5] = {artist[0], '\0'};
            draw_centered(r, font_title_, ch, Theme::TEXT_DIM,
                          ax + asz / 2, ay + asz / 2 - S(18));
        }
    }

    // ── Track text ────────────────────────────────────────────────────────────
    int tx = S(Theme::TEXT_X);
    int mw = w - tx - S(40);
    update_label(lc_title_,  r, font_title_, title.c_str(),  Theme::TEXT,     mw);
    update_label(lc_artist_, r, font_lg_,    artist.c_str(), Theme::TEXT_DIM, mw);
    draw_label(r, lc_title_,  tx, S(Theme::TITLE_Y));
    draw_label(r, lc_artist_, tx, S(Theme::ARTIST_Y));

    if (is_explicit) {
        // Small "E" pill badge to the right of the artist name, same baseline
        update_label(lc_expl_, r, font_sm_, "E", Theme::TEXT);
        int bpad = S(5);
        int bw   = lc_expl_.w + bpad * 2;
        int bh   = lc_expl_.h + S(2);
        int bx   = tx + lc_artist_.w + S(10);
        int by   = S(Theme::ARTIST_Y) + (lc_artist_.h - bh) / 2;
        fill_rounded(r, bx, by, bw, bh, S(3), SDL_Color{77, 77, 77, 220});
        draw_label(r, lc_expl_, bx + bpad, by + S(1));
    }

    // ── Spectrum visualizer ───────────────────────────────────────────────────
    // Bar heights are computed by spec_worker (CPU0, 30 Hz); just draw here.
    if (audio_src_) {
        float bars[SPEC_BARS];
        float loudness_db;
        {
            std::lock_guard<std::mutex> slk(spec_mu_);
            memcpy(bars, spec_bars_, sizeof(bars));
            loudness_db = loudness_db_;
        }

        // ── Loudness meter (15s EMA, dBFS) ───────────────────────────────────
        {
            // Sit flush against the right side, starting after the album art.
            // Bottom edge aligned with the art's bottom line (ART_Y + ART_SIZE).
            int mx     = S(Theme::TEXT_X);
            int mx_end = S(Theme::ART_X + 1200);  // same right edge as visualizer
            int mw     = mx_end - mx;
            int mh     = S(20);
            int my     = S(Theme::ART_Y + Theme::ART_SIZE) - mh;

            // Description + dBFS reading on the line above the bar
            char db_buf[16];
            snprintf(db_buf, sizeof(db_buf), "%.0f dBFS", loudness_db);
            draw_clipped(r, font_sm_, "Average Loudness (15s)", Theme::TEXT_DIM,
                         mx, my - S(24), mw - S(80));
            draw_clipped(r, font_sm_, db_buf, Theme::TEXT_HINT,
                         mx_end - S(78), my - S(24), S(78));

            // Track background
            SDL_SetRenderDrawColor(r, 30, 30, 30, 255);
            SDL_Rect bg = { mx, my, mw, mh };
            SDL_RenderFillRect(r, &bg);

            // Fill: map [-60, 0] dBFS → [0, mw], colour green→yellow→red
            float t = (loudness_db + 60.0f) / 60.0f;  // 0=silent, 1=0dBFS
            t = std::max(0.0f, std::min(1.0f, t));
            int fill_w = (int)(t * mw);
            if (fill_w > 0) {
                float hue = 120.0f * (1.0f - t);  // 120°=green → 0°=red
                SDL_Color fc = hsv_to_sdl(hue, 0.85f, 0.88f);
                SDL_SetRenderDrawColor(r, fc.r, fc.g, fc.b, 255);
                SDL_Rect fill = { mx, my, fill_w, mh };
                SDL_RenderFillRect(r, &fill);
            }
        }

        int vis_x   = S(40);
        int vis_bot = S(565);
        int vis_h   = S(130);
        int slot_w  = S(1200) / SPEC_BARS;
        int gap     = std::max(1, S(1));
        for (int b = 0; b < SPEC_BARS; ++b) {
            float norm  = std::min(1.0f, bars[b] / 0.25f);
            int   bar_h = (int)(norm * vis_h);
            if (bar_h < 1) continue;
            float hue        = (float)b / (SPEC_BARS - 1) * 270.0f;
            SDL_Color col    = hsv_to_sdl(hue, 0.85f, 0.90f);
            SDL_SetRenderDrawColor(r, col.r, col.g, col.b, 255);
            SDL_Rect rect = { vis_x + b * slot_w, vis_bot - bar_h,
                              slot_w - gap, bar_h };
            SDL_RenderFillRect(r, &rect);
        }
    }

    // ── Progress bar ─────────────────────────────────────────────────────────
    int bx = S(Theme::BAR_X), by = S(Theme::BAR_Y);
    int bw = S(Theme::BAR_W), bh = S(Theme::BAR_H);

    fill_rounded(r, bx, by, bw, bh, bh / 2, Theme::BAR_BG);
    if (dur_ms > 0) {
        int filled = (int)((int64_t)pos_ms * bw / dur_ms);
        fill_rounded(r, bx, by, std::min(filled, bw), bh, bh / 2, Theme::ACCENT);
    }

    char t_pos[16], t_dur[16];
    auto fmt = [](int ms, char *buf, size_t n) {
        int s = ms / 1000, m = s / 60; s %= 60;
        snprintf(buf, n, "%d:%02d", m, s);
    };
    fmt(pos_ms, t_pos, sizeof(t_pos)); fmt(dur_ms, t_dur, sizeof(t_dur));
    update_label(lc_pos_, r, font_sm_, t_pos, Theme::TEXT_DIM);
    update_label(lc_dur_, r, font_sm_, t_dur, Theme::TEXT_DIM);
    draw_label(r, lc_pos_, bx,              by + bh + S(4));
    draw_label(r, lc_dur_, bx + bw - S(50), by + bh + S(4));

    // ── Play indicator ────────────────────────────────────────────────────────
    {
        const SDL_Color &ac = Theme::ACCENT;
        SDL_SetRenderDrawColor(r, ac.r, ac.g, ac.b, ac.a);
        int cx = bx + (bw - S(50)) / 2;  // midpoint between pos and dur label anchors
        int cy = by + bh + S(50);

        if (playing) {
            // ▶  right-pointing filled triangle, scanline-filled
            int H = S(20), W = S(36);
            int lx = cx - W / 2, rx = cx + W / 2;
            for (int dy = -H; dy <= H; ++dy) {
                int ex = (dy <= 0) ? lx + (dy + H) * W / H
                                   : rx  - dy       * W / H;
                SDL_RenderDrawLine(r, lx, cy + dy, ex, cy + dy);
            }
        } else {
            // ⏸  two filled rectangles
            int bw2  = S(12), bh2 = S(38), half = S(5);
            SDL_Rect lb = {cx - half - bw2, cy - bh2 / 2, bw2, bh2};
            SDL_Rect rb = {cx + half,       cy - bh2 / 2, bw2, bh2};
            SDL_RenderFillRect(r, &lb);
            SDL_RenderFillRect(r, &rb);
        }
    }

    // ── Volume ────────────────────────────────────────────────────────────────
    char vol[16]; snprintf(vol, sizeof(vol), "Vol %d%%", volume);
    update_label(lc_vol_, r, font_sm_, vol, Theme::TEXT_DIM);
    draw_label(r, lc_vol_, w - S(80), S(12));

    // ── Shuffle / Repeat indicators ───────────────────────────────────────────
    SDL_Color shuf_col = shuffle       ? Theme::ACCENT : Theme::TEXT_HINT;
    SDL_Color rep_col  = repeat_mode   ? Theme::ACCENT : Theme::TEXT_HINT;
    char xtal_str_buf[12];
    if (xtal_on) snprintf(xtal_str_buf, sizeof(xtal_str_buf), "XTAL %d", xtal_str);
    else         snprintf(xtal_str_buf, sizeof(xtal_str_buf), "XTAL");
    SDL_Color xtal_col = xtal_on ? Theme::ACCENT : Theme::TEXT_HINT;

    update_label(lc_shuf_, r, font_sm_, "SHUF", shuf_col);
    update_label(lc_rep_,  r, font_sm_, repeat_mode == 2 ? "REP1" : "REP", rep_col);
    update_label(lc_xtal_, r, font_sm_, xtal_str_buf, xtal_col);
    draw_label(r, lc_shuf_, S(40),  S(12));
    draw_label(r, lc_rep_,  S(110), S(12));
    draw_label(r, lc_xtal_, S(170), S(12));

    // ── Controls hint ─────────────────────────────────────────────────────────
    draw_label(r, lc_bhint_, w / 2 - S(55), S(12));
}

// ── Drawing primitives ────────────────────────────────────────────────────────

void Display::draw_centered(SDL_Renderer *r, TTF_Font *f, const char *text,
                              SDL_Color col, int cx, int y) {
    if (!f || !text || !*text) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, text, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_Rect dst = {cx - surf->w / 2, y, surf->w, surf->h};
    SDL_FreeSurface(surf);
    if (!tex) return;
    SDL_RenderCopy(r, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
}

void Display::draw_clipped(SDL_Renderer *r, TTF_Font *f, const char *text,
                             SDL_Color col, int x, int y, int max_w) {
    if (!f || !text || !*text) return;
    std::string str = text;
    int tw = 0, th = 0;
    TTF_SizeUTF8(f, str.c_str(), &tw, &th);
    if (tw > max_w) {
        while (str.size() > 1) {
            str.pop_back();
            std::string cand = str + "\xe2\x80\xa6";  // …
            TTF_SizeUTF8(f, cand.c_str(), &tw, &th);
            if (tw <= max_w) { str = std::move(cand); break; }
        }
    }
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, str.c_str(), col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_Rect dst = {x, y, surf->w, surf->h};
    SDL_FreeSurface(surf);
    if (!tex) return;
    SDL_RenderCopy(r, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
}

void Display::fill_rounded(SDL_Renderer *r, int x, int y, int w, int h,
                             int radius, SDL_Color col) {
    SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);
    if (radius <= 0 || w < 2 * radius || h < 2 * radius) {
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(r, &rect);
        return;
    }
    SDL_Rect center = {x + radius, y,            w - 2 * radius, h           };
    SDL_Rect left   = {x,          y + radius,   radius,          h - 2 * radius};
    SDL_Rect right  = {x + w - radius, y + radius, radius,        h - 2 * radius};
    SDL_RenderFillRect(r, &center);
    SDL_RenderFillRect(r, &left);
    SDL_RenderFillRect(r, &right);
    int r2 = radius * radius;
    for (int dy = 0; dy <= radius; dy++) {
        int dx = (int)sqrt((double)(r2 - dy * dy));
        SDL_RenderDrawLine(r, x + radius - dx, y + radius - dy,
                              x + radius,      y + radius - dy);
        SDL_RenderDrawLine(r, x + w - radius,  y + radius - dy,
                              x + w - radius + dx, y + radius - dy);
        SDL_RenderDrawLine(r, x + radius - dx, y + h - radius + dy,
                              x + radius,      y + h - radius + dy);
        SDL_RenderDrawLine(r, x + w - radius,  y + h - radius + dy,
                              x + w - radius + dx, y + h - radius + dy);
    }
}

// ── Spectrum worker (CPU0, ~30 Hz) ───────────────────────────────────────────
// All expensive transcendental calls are precomputed on first entry.
// Hann window:  1024 cosf calls → done once, stored in hann[].
// Bin edges:    49 powf calls   → done once, stored in bins[].
// Magnitudes:   one sqrtf per bar (48) instead of one per bin (511).
// The render thread never calls into this code; it only copies spec_bars_.

void Display::spec_worker() {
    OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0);

    static constexpr int   FFT_N = 1024;
    static constexpr float SR    = 44100.0f;
    static constexpr float FMIN  = 60.0f;
    static constexpr float FMAX  = 16000.0f;

    static float hann[FFT_N];
    for (int i = 0; i < FFT_N; ++i)
        hann[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_N - 1)));

    static int bins[SPEC_BARS + 1];
    for (int b = 0; b <= SPEC_BARS; ++b) {
        float f = FMIN * powf(FMAX / FMIN, (float)b / SPEC_BARS);
        bins[b] = std::max(1, std::min(FFT_N / 2 - 1, (int)(f * FFT_N / SR)));
    }

    // Per-bar perceptual weight: power-law boost normalised at the geometric
    // mid-frequency (~980 Hz).  Exponent 0.4 gives ~10x gain from 60→16k Hz,
    // compensating the typical bass-heavy spectral tilt of music.
    static float weights[SPEC_BARS];
    {
        float f_mid = sqrtf(FMIN * FMAX);
        for (int b = 0; b < SPEC_BARS; ++b) {
            float fc = FMIN * powf(FMAX / FMIN, (b + 0.5f) / SPEC_BARS);
            weights[b] = powf(fc / f_mid, 0.4f);
        }
    }

    static float pcm[FFT_N];
    static float fre[FFT_N], fim[FFT_N];
    float smooth[SPEC_BARS] = {};

    // 15-second EMA loudness.  alpha = 1 - exp(-dt / tau), dt=16ms, tau=15000ms.
    const float LOUD_ALPHA = 1.0f - expf(-16.0f / 15000.0f);
    float loud_avg = 1e-6f;  // initialise to near-silence

    while (!spec_stop_.load()) {
        OSSleepTicks(OSMillisecondsToTicks(16));   // ~60 Hz

        if (!audio_src_) continue;

        audio_src_->get_pcm_snapshot(pcm, FFT_N);

        // RMS of raw PCM → 15s EMA loudness
        float sum2 = 0.0f;
        for (int i = 0; i < FFT_N; ++i) sum2 += pcm[i] * pcm[i];
        float rms = sqrtf(sum2 / FFT_N);
        loud_avg += LOUD_ALPHA * (rms - loud_avg);
        float db = 20.0f * log10f(std::max(loud_avg, 1e-6f));
        db = std::max(-60.0f, std::min(0.0f, db));

        for (int i = 0; i < FFT_N; ++i) { fre[i] = pcm[i] * hann[i]; fim[i] = 0.0f; }
        fft_inplace(fre, fim, FFT_N);

        // One sqrtf per bar (take peak bin in each band first, then sqrt once)
        float raw[SPEC_BARS];
        for (int b = 0; b < SPEC_BARS; ++b) {
            float peak = 0.0f;
            for (int k = bins[b]; k <= bins[b + 1]; ++k) {
                float m2 = fre[k]*fre[k] + fim[k]*fim[k];
                if (m2 > peak) peak = m2;
            }
            raw[b] = sqrtf(peak) * (2.0f / FFT_N) * weights[b];
        }

        for (int b = 0; b < SPEC_BARS; ++b) {
            float alpha = (raw[b] > smooth[b]) ? 0.90f : 0.18f;
            smooth[b] += alpha * (raw[b] - smooth[b]);
        }

        std::lock_guard<std::mutex> lk(spec_mu_);
        memcpy(spec_bars_, smooth, sizeof(spec_bars_));
        loudness_db_ = db;
    }
}

// ── Album art worker ──────────────────────────────────────────────────────────

size_t Display::curl_write(char *ptr, size_t sz, size_t n, void *ud) {
    auto *vec = static_cast<std::vector<uint8_t> *>(ud);
    vec->insert(vec->end(), reinterpret_cast<uint8_t *>(ptr),
                             reinterpret_cast<uint8_t *>(ptr) + sz * n);
    return sz * n;
}

void Display::art_worker() {
    OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0);
    std::string last_url;
    while (!art_stop_.load()) {
        std::string url;
        { std::lock_guard<std::mutex> lk(mu_); url = art_url_; }

        if (url.empty() || url == last_url || url == art_loaded_url_) {
            OSSleepTicks(OSMillisecondsToTicks(200));
            continue;
        }
        last_url = url;
        WHBLogPrintf("display: art fetch %s", url.c_str());

        std::vector<uint8_t> data;
        CURL *curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL,             url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   curl_write);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &data);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT,         10L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,  0L);
            curl_easy_perform(curl);
            curl_easy_cleanup(curl);
        }

        if (!data.empty()) {
            std::lock_guard<std::mutex> lk(art_mu_);
            art_pending_url_  = url;
            art_pending_data_ = std::move(data);
            art_ready_        = true;
        }
    }
}

// ── Roséverse OLV post card ───────────────────────────────────────────────────
// Sits in the text column (TEXT_X..BAR_X+BAR_W) below the artist name.

void Display::render_olv_card(SDL_Renderer *r, int w, int h) {
    std::string hdr, body;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!olv_visible_) return;
        hdr  = olv_header_;
        body = olv_body_;
    }
    if (hdr.empty() && body.empty() && !olv_memo_tex_) return;

    float s = w / 1280.0f;
    auto  S = [&](int v) { return (int)(v * s); };

    const int card_x  = S(Theme::TEXT_X);
    const int card_y  = S(Theme::ARTIST_Y + 60);  // ~230 at 1280 scale
    const int card_w  = S(Theme::BAR_X + Theme::BAR_W - Theme::TEXT_X);

    // Drawing rendered at half native size: 160×60 (preserves 320:120 = 8:3 aspect)
    const int draw_w = S(160), draw_h = S(60);

    // Card height grows to fit: header row + optional drawing + optional body text
    int card_h = S(22);  // header
    if (olv_memo_tex_) card_h += draw_h + S(4);
    if (!body.empty())  card_h += S(22);
    card_h += S(6);  // bottom padding

    // Dark semi-transparent card background
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 20, 10, 30, 210);
    SDL_Rect bg = { card_x, card_y, card_w, card_h };
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    // Accent left-edge bar
    SDL_SetRenderDrawColor(r, Theme::ACCENT.r, Theme::ACCENT.g, Theme::ACCENT.b, 255);
    SDL_Rect bar = { card_x, card_y, S(3), card_h };
    SDL_RenderFillRect(r, &bar);

    const int tx = card_x + S(12);
    const int mw = card_w - S(16);
    int cy = card_y + S(6);

    // Header: "Miiverse  @name  :)" — accent colour, small font
    if (font_sm_) {
        std::string full_hdr = "Miiverse  " + hdr;
        update_label(lc_olv_header_, r, font_sm_, full_hdr.c_str(), Theme::ACCENT, mw);
        draw_label(r, lc_olv_header_, tx, cy);
    }
    cy += S(16);

    // Drawing (if present)
    if (olv_memo_tex_) {
        SDL_Rect dst = { tx, cy, draw_w, draw_h };
        SDL_RenderCopy(r, olv_memo_tex_, nullptr, &dst);
        cy += draw_h + S(4);
    }

    // Body text — white, medium font, truncated to card width
    if (font_md_ && !body.empty()) {
        update_label(lc_olv_body_, r, font_md_, body.c_str(), Theme::TEXT, mw);
        draw_label(r, lc_olv_body_, tx, cy);
    }
}

} // namespace UI
