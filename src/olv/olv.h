#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace OLV {

static constexpr uint32_t COMMUNITY_ID = 157;

// 0=normal 1=happy 2=wink 3=surprised 4=frustrated 5=sad
static constexpr const char *FEELING_STR[] = {
    "", ":)", ";)", ":O", ">:(", ":("
};

struct Post {
    std::string body;
    std::string screen_name;
    std::string post_id;      // nn::olv post identifier; empty if unavailable
    int         feeling;      // 0–5
    uint32_t    position_ms;  // track position when posted; 0 = no metadata
    uint32_t    duration_ms;  // track duration at post time;  0 = no metadata
    // Raw TGA bytes from GetBodyMemo (320×120 BGRA from lower-left); empty = text-only post.
    std::vector<uint8_t> memo;
};

// Detect Roséverse via discovery endpoint, load nn_olv.rpl, initialise.
// Blocks on network. Returns true iff everything succeeded.
// Must be called from a background thread (never the main/render thread).
bool init();

void shutdown();

bool is_available();

// Synchronously fetch up to limit posts from community_id.
// If search_key is non-empty, the server filters to posts with that key.
// Blocks. Returns empty vector on any failure.
std::vector<Post> fetch_posts(uint32_t community_id, uint32_t limit,
                              const std::string &search_key = {});

// Open the Miiverse post-creation applet with body_utf8 pre-filled.
// title sets the topic tag (pass "Song - Artist"); search_key (Spotify track ID) is attached for
// per-song post lookup. is_explicit=true sets the IS_SPOILER flag.
// position_ms / duration_ms are embedded in the post's hidden app-data binary so that
// clients can show posts anchored to their track timestamp. Pass 0/0 if unknown.
// Blocking — returns after the user posts or cancels.
void open_post_applet(const std::string &body_utf8, bool is_explicit,
                      const std::string &title, const std::string &search_key,
                      uint32_t position_ms, uint32_t duration_ms);

// Open the Miiverse portal via StartPortalApp. If post_id is non-empty and the symbol
// resolved correctly, navigates directly to that post; otherwise opens community 157.
// Falls back to SYSSwitchTo(SYSAPP_PFID_MIIVERSE) if StartPortalApp symbols are missing.
void open_overlay(const std::string &post_id = {});

} // namespace OLV
