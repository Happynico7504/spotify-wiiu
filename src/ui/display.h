#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

namespace Connect { class AudioPipeline; }

namespace UI {

// Renders a minimal now-playing screen to TV (1280×720) and GamePad DRC (854×480).
//
// Thread safety: set_* methods are safe to call from any thread.
// render() must be called from the main thread each frame.
//
// Album art is fetched on a background thread via curl; the placeholder
// (coloured box + first letter) shows until art arrives.

class Display {
public:
    Display();
    ~Display();

    bool init(const char *font_path);
    void shutdown();
    void render();   // call each frame from main thread

    // ── state setters (thread-safe) ──────────────────────────────────────────
    void set_waiting();   // "Waiting for Spotify…" before any session
    void set_error(const std::string &msg);  // show error on the waiting screen
    void set_track(const std::string &title, const std::string &artist,
                   const std::string &album_art_url, bool is_explicit = false);
    void set_progress(int position_ms, int duration_ms, bool playing);
    void set_volume(int pct);              // 0–100
    void set_shuffle(bool on);
    void set_repeat(int mode);  // 0=off, 1=repeat-context, 2=repeat-track
    void set_crystalizer(bool on, int strength);  // strength 1–10
    void toggle_controls();
    void set_audio(Connect::AudioPipeline *audio);  // non-owning; must outlive Display

    // ── Roséverse OLV post card ──────────────────────────────────────────────
    struct OLVPost {
        std::string          body;
        std::string          screen_name;
        int                  feeling = 0;  // 0–5
        std::vector<uint8_t> memo;         // raw TGA 320×120 BGRA from lower-left; empty = text-only
    };
    // Pass nullptr to hide the card.
    void set_olv_post(const OLVPost *post);

    // ── cached text label ────────────────────────────────────────────────────
    // Texture is recreated only when str or col changes. max_w=-1 means
    // no clipping (draw_centered path); otherwise text is truncated with '…'.
    struct CachedLabel {
        std::string  str;
        SDL_Color    col  = {0,0,0,0};
        int          max_w = -1;
        SDL_Texture *tex  = nullptr;
        int w = 0, h = 0;
        void free() { if (tex) { SDL_DestroyTexture(tex); tex = nullptr; } }
    };

private:
    // ── rendering ────────────────────────────────────────────────────────────
    void render_to(SDL_Renderer *r, int w, int h);
    void render_waiting(SDL_Renderer *r, int w, int h);
    void render_playing(SDL_Renderer *r, int w, int h);
    void render_controls(SDL_Renderer *r, int w, int h);
    void render_olv_card(SDL_Renderer *r, int w, int h);

    // Ensure lbl holds an up-to-date texture for (text, col, max_w).
    // Returns true if the texture was (re)created this call.
    bool update_label(CachedLabel &lbl, SDL_Renderer *r, TTF_Font *f,
                      const char *text, SDL_Color col, int max_w = -1);
    // Draw a cached label at (x,y), or centred at cx if cx >= 0.
    static void draw_label(SDL_Renderer *r, const CachedLabel &lbl,
                           int x, int y, int cx = -1);

    // draw text centered at (cx, y)  — kept for controls overlay (not cached)
    void draw_centered(SDL_Renderer *r, TTF_Font *f, const char *text,
                       SDL_Color col, int cx, int y);
    // draw text left-aligned, clipped to max_w  — kept for controls overlay
    void draw_clipped(SDL_Renderer *r, TTF_Font *f, const char *text,
                      SDL_Color col, int x, int y, int max_w);
    // draw filled rounded rectangle
    void fill_rounded(SDL_Renderer *r, int x, int y, int w, int h,
                      int radius, SDL_Color col);

    // ── album art worker ─────────────────────────────────────────────────────
    void art_worker();
    static size_t curl_write(char *ptr, size_t sz, size_t n, void *ud);

    // ── SDL objects ──────────────────────────────────────────────────────────
    SDL_Window   *tv_win_   = nullptr;
    SDL_Renderer *tv_ren_   = nullptr;

    TTF_Font *font_title_ = nullptr;   // ~17pt
    TTF_Font *font_lg_    = nullptr;   // ~14pt
    TTF_Font *font_md_    = nullptr;   // ~11pt
    TTF_Font *font_sm_    = nullptr;   //  ~9pt

    // ── pre-baked background gradient ────────────────────────────────────────
    // 2×720 static texture; SDL_RenderCopy stretches it to fill the screen.
    // Created once in init(); replaces 720 per-frame SDL_RenderDrawLine calls.
    SDL_Texture *bg_tex_ = nullptr;

    // ── text label caches (main-thread only) ─────────────────────────────────
    // Textures are recreated only when the displayed string or color changes.
    CachedLabel lc_title_, lc_artist_;        // track info
    CachedLabel lc_expl_;                    // "E" explicit badge text
    CachedLabel lc_pos_,   lc_dur_;           // M:SS timestamps
    CachedLabel lc_vol_;                      // "Vol N%"
    CachedLabel lc_shuf_,  lc_rep_, lc_xtal_; // SHUF / REP / XTAL (color encodes on/off)
    CachedLabel lc_bhint_;                    // "B: Controls" — static, baked in init
    CachedLabel lc_wait_[2];                  // waiting-screen lines
    CachedLabel lc_olv_header_;               // "Roséverse  @name  :)"
    CachedLabel lc_olv_body_;                 // post body text

    // ── display state (guarded by mu_) ───────────────────────────────────────
    std::mutex  mu_;
    bool        waiting_    = true;
    std::string error_msg_;              // non-empty → show error on waiting screen
    std::string snap_error_;             // render-thread copy, updated each frame under mu_
    std::string title_;
    std::string artist_;
    bool        explicit_   = false;
    std::string art_url_;
    int         pos_ms_     = 0;
    int         dur_ms_     = 0;
    bool        playing_    = false;
    int         volume_     = 100;
    bool        shuffle_          = false;
    int         repeat_mode_      = 0;     // 0=off, 1=context, 2=track
    bool        crystal_enabled_  = false;
    int         crystal_strength_ = 5;
    bool        controls_         = false;
    bool        olv_visible_      = false;
    std::string olv_body_;
    std::string olv_header_;  // pre-formatted "@name  :)" line
    // Pending drawing texture (set from any thread under mu_; created on main thread in render())
    std::vector<uint8_t> olv_memo_pending_;
    bool                 olv_memo_dirty_ = false;
    // Drawing texture — main-thread only, NOT under mu_
    SDL_Texture         *olv_memo_tex_   = nullptr;

    // ── spectrum visualizer ───────────────────────────────────────────────────
    Connect::AudioPipeline *audio_src_ = nullptr;
    static constexpr int SPEC_BARS = 48;

    // Background worker (CPU0, ~30 Hz) writes here; render thread reads.
    std::mutex spec_mu_;
    float      spec_bars_[SPEC_BARS] = {};   // published by worker, read by render
    float      loudness_db_ = -60.0f;        // 15s EMA loudness in dBFS, same lock

    std::thread           spec_thread_;
    std::atomic<bool>     spec_stop_{false};
    void spec_worker();                      // defined in display.cpp

    // ── album art ────────────────────────────────────────────────────────────
    SDL_Texture *art_tex_   = nullptr;
    std::string  art_loaded_url_;   // URL currently in art_tex_

    std::thread      art_thread_;
    std::atomic<bool> art_stop_{false};
    // art worker reads art_url_ under mu_, signals back via art_pending_*
    std::mutex            art_mu_;
    std::string           art_pending_url_;
    std::vector<uint8_t>  art_pending_data_;
    bool                  art_ready_ = false;
};

} // namespace UI
