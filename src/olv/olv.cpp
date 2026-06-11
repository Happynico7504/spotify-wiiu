#include "olv.h"

#include <cstring>
#include <algorithm>
#include <string>

#include <wut.h>
#include <coreinit/dynload.h>
#include <sysapp/switch.h>
#include <whb/log.h>


#include <nn/ac.h>
#include <nn/act.h>
#include <nsysnet/nssl.h>
#include <memory>

namespace OLV {

// ── nn::olv::InitializeParam — 0x40 bytes ────────────────────────────────────
struct alignas(4) InitParam {
    uint32_t flags;       // 0x00
    uint32_t reportTypes; // 0x04
    void    *work;        // 0x08
    uint32_t workSize;    // 0x0C  must be >= 0x40000 (256 KB)
    void    *sysArgs;     // 0x10
    uint32_t sysArgsSize; // 0x14
    uint8_t  _pad[0x28];
};
static_assert(sizeof(InitParam) == 0x40);

alignas(32) static uint8_t s_work[0x40000]; // 256 KB

// ── DownloadPostDataListParam — passed to DownloadPostDataList ────────────────
struct alignas(4) DownloadParam {
    uint32_t flags;           // 0x000
    uint32_t communityId;     // 0x004
    uint8_t  _pad8[0x034];    // 0x008–0x03B
    uint32_t postDataMaxNum;  // 0x03C
    uint8_t  _padRest[0xFC0]; // 0x040–0xFFF
};
static_assert(sizeof(DownloadParam) == 0x1000);

// ── DownloadedPostData / DownloadedTopicData layout ───────────────────────────
// Struct sizes and field offsets verified from Cemu OS/libs/nn_olv source.
// DownloadedDataBase (base of DownloadedPostData) field offsets:
static constexpr uint32_t k_PostDataSize  = 0xC208; // sizeof DownloadedPostData
static constexpr uint32_t k_TopicDataSize = 0x1000; // sizeof DownloadedTopicData
static constexpr uint32_t k_OffFeeling    = 0x0030; // sint8
static constexpr uint32_t k_OffBodyText   = 0x003C; // uint16[256] UTF-16
static constexpr uint32_t k_OffBodyLen    = 0x023C; // uint32 character count
static constexpr uint32_t k_OffNickname   = 0xAAE0; // uint16[16] UTF-16

// ── Module state ──────────────────────────────────────────────────────────────

static OSDynLoad_Module s_handle    = nullptr;
static bool             s_available = false;

// nn::olv::DownloadPostDataList — native post fetch via system HTTP stack
using FnDownloadPosts    = int32_t (*)(void *, void *, uint32_t *, uint32_t, const DownloadParam *);
using FnCtor             = void    (*)(void *);
using FnDLSetSearchKey   = int32_t (*)(void *, const uint16_t *);  // DownloadPostDataListParam::SetSearchKey (UTF-16)
static FnDownloadPosts   s_fn_download         = nullptr;
static FnCtor            s_fn_ctor_post        = nullptr;
static FnCtor            s_fn_ctor_topic       = nullptr;
static FnDLSetSearchKey  s_fn_dl_set_search    = nullptr;

// nn::olv::UploadPostDataByPostApp — interactive post creation applet
using FnSetWork        = void    (*)(void *, uint8_t *, uint32_t);
using FnSetBodyText    = void    (*)(void *, const uint16_t *);  // wchar_t* = uint16_t* on Wii U
using FnSetFlags       = void    (*)(void *, uint32_t);
using FnSetTopicTag    = void    (*)(void *, const uint16_t *);  // UTF-16 topic label
using FnSetSearchKey   = void    (*)(void *, const uint16_t *, uint8_t);  // UTF-16 search key + index
using FnSetCommunityId = void    (*)(void *, uint32_t);
using FnUploadPost     = int32_t (*)(const void *);
static FnCtor          s_fn_ctor_upload    = nullptr;
static FnSetWork       s_fn_set_work       = nullptr;
static FnSetBodyText   s_fn_set_body       = nullptr;
static FnSetFlags      s_fn_set_flags      = nullptr;
static FnSetTopicTag   s_fn_set_topic      = nullptr;
static FnSetSearchKey  s_fn_set_search     = nullptr;
static FnSetCommunityId s_fn_set_community = nullptr;
static FnUploadPost    s_fn_upload_post    = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Convert a UTF-16 string (max_chars code units, null-terminated) to UTF-8.
// BMP-only; surrogate pairs produce replacement characters.
static std::string utf16_to_utf8(const uint16_t *src, uint32_t max_chars) {
    std::string out;
    for (uint32_t i = 0; i < max_chars && src[i]; ++i) {
        uint32_t c = src[i];
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Convert UTF-8 to null-terminated UTF-16 (BMP only, max_chars code units before null).
static std::vector<uint16_t> utf8_to_utf16(const std::string &utf8, uint32_t max_chars) {
    std::vector<uint16_t> out;
    for (size_t i = 0; i < utf8.size() && out.size() < max_chars; ) {
        uint8_t b = (uint8_t)utf8[i];
        uint32_t c;
        if      (b < 0x80) { c = b;                                                         i += 1; }
        else if (b < 0xE0) { c = (b & 0x1F) << 6  | ((uint8_t)utf8[i+1] & 0x3F);           i += 2; }
        else               { c = (b & 0x0F) << 12 | ((uint8_t)utf8[i+1] & 0x3F) << 6
                                                   | ((uint8_t)utf8[i+2] & 0x3F);            i += 3; }
        out.push_back(static_cast<uint16_t>(c));
    }
    out.push_back(0);
    return out;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool init() {
    // Initialize the full stack in dependency order, matching the sequence used
    // by the GetMyMiiverseToken Roséverse reference tool.
    // Acquire the Miiverse service token.
    // Inkay-Roseverse intercepts AcquireIndependentServiceToken for OLV
    // client ID "87cd32617f1985439ea608c2746e4610" and returns a Roséverse token.
    {
        using FnAIST = int32_t (*)(char *, const char *, uint32_t, bool, bool);
        FnAIST fn_aist = nullptr;
        OSDynLoad_Module act_handle = nullptr;
        if (OSDynLoad_Acquire("nn_act.rpl", &act_handle) == OS_DYNLOAD_OK) {
            OSDynLoad_FindExport(act_handle, OS_DYNLOAD_EXPORT_FUNC,
                "AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUibT4",
                reinterpret_cast<void **>(&fn_aist));
            OSDynLoad_Release(act_handle);
        }
        if (!fn_aist) {
            WHBLogPrint("olv: AIST symbol not found");
            return false;
        }
        char token[513] = {};
        int32_t rc = fn_aist(token, "87cd32617f1985439ea608c2746e4610",
                             3600, false, false);
        WHBLogPrintf("olv: AIST → 0x%08X", (uint32_t)rc);
        if (rc != 0 || token[0] == '\0') {
            WHBLogPrint("olv: token acquisition failed");
            return false;
        }
    }

    // Load nn_olv.rpl.
    if (OSDynLoad_Acquire("nn_olv.rpl", &s_handle) != OS_DYNLOAD_OK) {
        WHBLogPrint("olv: nn_olv.rpl acquire failed");
        return false;
    }

    // Resolve post-download path.
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "DownloadPostDataList__Q2_2nn3olvFPQ3_2nn3olv19DownloadedTopicDataPQ3_2nn3olv18DownloadedPostDataPUiUiPCQ3_2nn3olv25DownloadPostDataListParam",
        reinterpret_cast<void **>(&s_fn_download));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "__ct__Q3_2nn3olv18DownloadedPostDataFv",
        reinterpret_cast<void **>(&s_fn_ctor_post));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "__ct__Q3_2nn3olv19DownloadedTopicDataFv",
        reinterpret_cast<void **>(&s_fn_ctor_topic));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "SetSearchKey__Q3_2nn3olv25DownloadPostDataListParamFPCw",
        reinterpret_cast<void **>(&s_fn_dl_set_search));
    WHBLogPrintf("olv: native download=%s ctor_post=%s ctor_topic=%s dl_search=%s",
                 s_fn_download      ? "ok" : "missing",
                 s_fn_ctor_post     ? "ok" : "missing",
                 s_fn_ctor_topic    ? "ok" : "missing",
                 s_fn_dl_set_search ? "ok" : "missing");

    // Post-creation applet symbols.
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "__ct__Q3_2nn3olv28UploadPostDataByPostAppParamFv",
        reinterpret_cast<void **>(&s_fn_ctor_upload));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "SetWork__Q3_2nn3olv28UploadPostDataByPostAppParamFPUcUi",
        reinterpret_cast<void **>(&s_fn_set_work));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "SetBodyText__Q3_2nn3olv15UploadParamBaseFPCw",
        reinterpret_cast<void **>(&s_fn_set_body));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "SetFlags__Q3_2nn3olv28UploadPostDataByPostAppParamFUi",
        reinterpret_cast<void **>(&s_fn_set_flags));
    if (!s_fn_set_flags)
        OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
            "SetFlags__Q3_2nn3olv15UploadParamBaseFUi",
            reinterpret_cast<void **>(&s_fn_set_flags));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "SetTopicTag__Q3_2nn3olv15UploadParamBaseFPCw",
        reinterpret_cast<void **>(&s_fn_set_topic));
    if (!s_fn_set_topic)
        OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
            "SetTopicTag__Q3_2nn3olv28UploadPostDataByPostAppParamFPCw",
            reinterpret_cast<void **>(&s_fn_set_topic));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "SetSearchKey__Q3_2nn3olv19UploadPostDataParamFPCwUc",
        reinterpret_cast<void **>(&s_fn_set_search));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "SetCommunityId__Q3_2nn3olv19UploadPostDataParamFUi",
        reinterpret_cast<void **>(&s_fn_set_community));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "UploadPostDataByPostApp__Q2_2nn3olvFPCQ3_2nn3olv28UploadPostDataByPostAppParam",
        reinterpret_cast<void **>(&s_fn_upload_post));
    WHBLogPrintf("olv: post applet ctor=%s work=%s body=%s flags=%s topic=%s search=%s community=%s upload=%s",
                 s_fn_ctor_upload    ? "ok" : "missing",
                 s_fn_set_work       ? "ok" : "missing",
                 s_fn_set_body       ? "ok" : "missing",
                 s_fn_set_flags      ? "ok" : "missing",
                 s_fn_set_topic      ? "ok" : "missing",
                 s_fn_set_search     ? "ok" : "missing",
                 s_fn_set_community  ? "ok" : "missing",
                 s_fn_upload_post    ? "ok" : "missing");

    // Initialize the network/SSL/account stack before calling nn_olv::Initialize.
    // Must come after the RPL load — calling these on a background thread before
    // OSDynLoad_Acquire races against Aroma's already-live account state.
    nn::ac::Initialize();
    {
        nn::ac::ConfigIdNum configId;
        nn::ac::GetStartupId(&configId);
        nn::ac::Connect(configId);
    }
    NSSLInit();
    nn::act::Initialize();

    // Attempt Initialize with 256 KB work buffer (minimum per reference tool).
    {
        using FnInit = int32_t (*)(const InitParam *);
        FnInit fn_init = nullptr;
        OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
            "Initialize__Q2_2nn3olvFPCQ3_2nn3olv15InitializeParam",
            reinterpret_cast<void **>(&fn_init));

        if (fn_init) {
            memset(s_work, 0, sizeof(s_work));
            InitParam param = {};
            param.work     = s_work;
            param.workSize = sizeof(s_work);
            int32_t r = fn_init(&param);
            WHBLogPrintf("olv: Initialize → 0x%08X", (uint32_t)r);
            // 0x01100080 is Roséverse's success code.
            if (r == 0 || (uint32_t)r == 0x01100080u) {
                WHBLogPrint("olv: ready");
                s_available = true;
                return true;
            }
        }
    }

    WHBLogPrint("olv: Initialize failed");
    return false;
}

void shutdown() {
    s_available = false;
    if (s_handle) { OSDynLoad_Release(s_handle); s_handle = nullptr; }
    nn::act::Finalize();
    NSSLFinish();
    nn::ac::Finalize();
}

bool is_available() { return s_available; }

std::vector<Post> fetch_posts(uint32_t community_id, uint32_t limit,
                              const std::string &search_key) {
    if (!s_available) return {};

    // ── Native nn_olv path (uses system HTTP stack, respects Inkay DNS) ────────
    if (s_fn_download && s_fn_ctor_post && s_fn_ctor_topic) {
        std::unique_ptr<uint8_t[]> topic(new (std::nothrow) uint8_t[k_TopicDataSize]());
        std::unique_ptr<uint8_t[]> posts(new (std::nothrow) uint8_t[k_PostDataSize * limit]());
        if (topic && posts) {
            s_fn_ctor_topic(topic.get());
            for (uint32_t i = 0; i < limit; ++i)
                s_fn_ctor_post(posts.get() + i * k_PostDataSize);

            DownloadParam param = {};
            param.communityId    = community_id;
            param.postDataMaxNum = limit;
            if (!search_key.empty() && s_fn_dl_set_search) {
                auto sk16 = utf8_to_utf16(search_key, 64);
                s_fn_dl_set_search(&param, sk16.data());
            }

            uint32_t num_out = 0;
            int32_t rc = s_fn_download(topic.get(), posts.get(), &num_out, limit, &param);
            WHBLogPrintf("olv: DownloadPostDataList → 0x%08X, got %u", (uint32_t)rc, num_out);

            if ((rc == 0 || (uint32_t)rc == 0x01100080u) && num_out > 0) {
                std::vector<Post> out;
                for (uint32_t i = 0; i < num_out; ++i) {
                    const uint8_t *p = posts.get() + i * k_PostDataSize;
                    uint32_t body_len = *reinterpret_cast<const uint32_t *>(p + k_OffBodyLen);
                    body_len = std::min(body_len, uint32_t{256});
                    Post post;
                    post.body = utf16_to_utf8(
                        reinterpret_cast<const uint16_t *>(p + k_OffBodyText), body_len);
                    post.screen_name = utf16_to_utf8(
                        reinterpret_cast<const uint16_t *>(p + k_OffNickname), 16);
                    post.feeling = std::max(0, std::min(5,
                        static_cast<int>(static_cast<int8_t>(p[k_OffFeeling]))));
                    out.push_back(std::move(post));
                }
                WHBLogPrintf("olv: fetch ok, %zu posts", out.size());
                return out;
            }
        }
    }

    return {};
}

void open_post_applet(const std::string &body_utf8, bool is_explicit,
                      const std::string &title, const std::string &search_key) {
    if (!s_fn_ctor_upload || !s_fn_set_work || !s_fn_set_body || !s_fn_upload_post) {
        WHBLogPrint("olv: post applet symbols not available");
        return;
    }

    // UploadPostDataByPostAppParam — conservative size estimate for the param header.
    // The actual data lives in the work buffer; the struct itself just holds metadata.
    static constexpr uint32_t k_UploadParamSize = 0x80;
    alignas(32) static uint8_t s_upload_work[0x40000];  // 256 KB work buffer
    alignas(4)  static uint8_t s_upload_param[k_UploadParamSize];

    memset(s_upload_param, 0, sizeof(s_upload_param));
    s_fn_ctor_upload(s_upload_param);
    s_fn_set_work(s_upload_param, s_upload_work, sizeof(s_upload_work));
    if (s_fn_set_community)
        s_fn_set_community(s_upload_param, COMMUNITY_ID);

    // Convert body text to UTF-16 (Wii U wchar_t is 2 bytes; 200-char Miiverse limit).
    auto body16 = utf8_to_utf16(body_utf8, 200);
    s_fn_set_body(s_upload_param, body16.data());

    // Topic tag: song title (UTF-16) — groups posts by track on Roséverse.
    if (s_fn_set_topic) {
        auto title16 = utf8_to_utf16(title, 64);
        s_fn_set_topic(s_upload_param, title16.data());
    }

    // Search key: Spotify track ID — used to find all posts for a specific song.
    if (s_fn_set_search && !search_key.empty()) {
        auto sk16 = utf8_to_utf16(search_key, 64);
        s_fn_set_search(s_upload_param, sk16.data(), 0);
    }

    // IS_SPOILER in the upload param flags is bit 0 (0x1), distinct from the
    // DownloadedDataBase read-side flag (0x200).
    if (is_explicit && s_fn_set_flags)
        s_fn_set_flags(s_upload_param, 0x00000001u);

    int32_t rc = s_fn_upload_post(s_upload_param);
    WHBLogPrintf("olv: UploadPostDataByPostApp → 0x%08X", (uint32_t)rc);
}

void open_overlay() {
    SYSSwitchTo(SYSAPP_PFID_MIIVERSE);
}

} // namespace OLV
