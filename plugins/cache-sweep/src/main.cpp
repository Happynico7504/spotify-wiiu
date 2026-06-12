#include <wups.h>
#include <wups/config/WUPSConfigCategory.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemIntegerRange.h>

#include <coreinit/thread.h>
#include <coreinit/time.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <atomic>

WUPS_PLUGIN_NAME("Spotify Cache Sweep");
WUPS_PLUGIN_DESCRIPTION("Sweeps stale Spotify Wii U audio cache at boot and on a configurable interval");
WUPS_PLUGIN_VERSION("1.0.0");
WUPS_PLUGIN_AUTHOR("Nico Christmann");
WUPS_PLUGIN_LICENSE("MIT");

WUPS_USE_WUT_DEVOPTAB();

// ── Constants ─────────────────────────────────────────────────────────────────

#define CACHE_BASE   "/vol/external01/spotify_cache"
#define FLAG_FILE    CACHE_BASE "/.sweep_now"
#define CONFIG_FILE  CACHE_BASE "/.plugin_config"
#define MAX_AGE_SECS (3 * 24 * 60 * 60)  // 3 days, matches WUHB

// ── Config (plain text file next to the cache) ────────────────────────────────

static bool    s_enabled      = true;
static int32_t s_interval_min = 60;

static void load_config() {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return;
    int enabled = 1;
    fscanf(f, "enabled=%d\ninterval_min=%d", &enabled, &s_interval_min);
    fclose(f);
    s_enabled = (bool)enabled;
    if (s_interval_min < 1)    s_interval_min = 1;
    if (s_interval_min > 1440) s_interval_min = 1440;
}

static void save_config() {
    mkdir(CACHE_BASE, 0755);
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) return;
    fprintf(f, "enabled=%d\ninterval_min=%d\n", (int)s_enabled, s_interval_min);
    fclose(f);
}

// ── Sweep logic ───────────────────────────────────────────────────────────────

static int do_sweep() {
    int evicted = 0;
    time_t cutoff = time(nullptr) - MAX_AGE_SECS;

    DIR *base = opendir(CACHE_BASE);
    if (!base) return 0;

    struct dirent *tde;
    while ((tde = readdir(base)) != nullptr) {
        if (tde->d_name[0] == '.') continue;

        char tdir[512], ts_path[560];
        snprintf(tdir,    sizeof(tdir),    "%s/%s",         CACHE_BASE, tde->d_name);
        snprintf(ts_path, sizeof(ts_path), "%s/.timestamp", tdir);

        FILE *tf = fopen(ts_path, "r");
        if (!tf) continue;
        long long cached_at = 0, last_played = 0;
        fscanf(tf, "%lld\n%lld", &cached_at, &last_played);
        fclose(tf);

        if ((time_t)last_played >= cutoff) continue;

        DIR *td = opendir(tdir);
        if (td) {
            struct dirent *cde;
            while ((cde = readdir(td)) != nullptr) {
                if (cde->d_name[0] == '.') continue;
                char path[768];
                snprintf(path, sizeof(path), "%s/%s", tdir, cde->d_name);
                unlink(path);
            }
            closedir(td);
        }
        unlink(ts_path);
        rmdir(tdir);
        ++evicted;
    }
    closedir(base);

    return evicted;
}

// ── Background thread ─────────────────────────────────────────────────────────

static OSThread          s_thread;
static uint8_t           s_stack[16 * 1024];
static std::atomic<bool> s_stop{false};

static int sweep_thread_fn(int argc, const char **argv) {
    if (s_enabled)
        do_sweep();

    while (!s_stop.load()) {
        int secs = s_interval_min * 60;
        for (int i = 0; i < secs && !s_stop.load(); ++i) {
            OSSleepTicks(OSMillisecondsToTicks(1000));

            FILE *f = fopen(FLAG_FILE, "r");
            if (f) {
                fclose(f);
                unlink(FLAG_FILE);
                if (s_enabled)
                    do_sweep();
            }
        }

        if (!s_stop.load() && s_enabled)
            do_sweep();
    }

    return 0;
}

// ── WUPS config menu ──────────────────────────────────────────────────────────

static void on_enabled_changed(ConfigItemBoolean *, bool value) {
    s_enabled = value;
    save_config();
}

static void on_interval_changed(ConfigItemIntegerRange *, int32_t value) {
    s_interval_min = value;
    save_config();
}

static WUPSConfigAPICallbackStatus config_opened(WUPSConfigCategoryHandle root_handle) {
    WUPSConfigCategory root(root_handle);
    try {
        root.add(WUPSConfigItemBoolean::Create(
            "enabled", "Auto-sweep enabled",
            true, s_enabled, on_enabled_changed));

        root.add(WUPSConfigItemIntegerRange::Create(
            "interval_min", "Sweep interval (minutes)",
            60, s_interval_min, 1, 1440,
            on_interval_changed));
    } catch (const std::exception &e) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

static void config_closed() {}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

INITIALIZE_PLUGIN() {
    load_config();

    WUPSConfigAPIOptionsV1 opts = {.name = "Spotify Cache Sweep"};
    WUPSConfigAPI_Init(opts, config_opened, config_closed);

    s_stop.store(false);
    OSCreateThread(&s_thread, sweep_thread_fn, 0, nullptr,
                   s_stack + sizeof(s_stack), sizeof(s_stack),
                   16, OS_THREAD_ATTRIB_AFFINITY_CPU0);
    OSSetThreadName(&s_thread, "spotify-cache-sweep");
    OSResumeThread(&s_thread);
}

DEINITIALIZE_PLUGIN() {
    s_stop.store(true);
    OSJoinThread(&s_thread, nullptr);
}
