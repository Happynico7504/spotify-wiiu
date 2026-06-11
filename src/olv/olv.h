#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace OLV {

// 0 = general/default community on Roséverse.
// Update to the dedicated Spotify community ID once assigned.
static constexpr uint32_t COMMUNITY_ID = 0;

// 0=normal 1=happy 2=wink 3=surprised 4=frustrated 5=sad
static constexpr const char *FEELING_STR[] = {
    "", ":)", ";)", ":O", ">:(", ":("
};

struct Post {
    std::string body;
    std::string screen_name;
    int         feeling;  // 0–5
};

// Detect Roséverse via discovery endpoint, load nn_olv.rpl, initialise.
// Blocks on network. Returns true iff everything succeeded.
// Must be called from a background thread (never the main/render thread).
bool init();

void shutdown();

bool is_available();

// Synchronously fetch up to limit posts from community_id.
// Blocks. Returns empty vector on any failure.
std::vector<Post> fetch_posts(uint32_t community_id, uint32_t limit);

// Open the Miiverse post-creation applet with body_utf8 pre-filled.
// title sets the topic tag; search_key (Spotify track ID) is attached for
// per-song post lookup. is_explicit=true sets the IS_SPOILER flag.
// Blocking — returns after the user posts or cancels.
void open_post_applet(const std::string &body_utf8, bool is_explicit,
                      const std::string &title, const std::string &search_key);

// Open the Miiverse overlay (redirected to Roséverse by Inkay-Roseverse).
void open_overlay();

} // namespace OLV
