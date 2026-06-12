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
    uint32_t workSize;    // 0x0C
    void    *sysArgs;     // 0x10
    uint32_t sysArgsSize; // 0x14
    uint8_t  _pad[0x28];
};
static_assert(sizeof(InitParam) == 0x40);

alignas(32) static uint8_t s_work[0x200000]; // 2 MB — download responses can be large

// ── DownloadPostDataListParam — opaque; all fields set via nn_olv setters ─────
struct alignas(4) DownloadParam { uint8_t _data[0x1000]; };
static_assert(sizeof(DownloadParam) == 0x1000);

// ── DownloadedPostData / DownloadedTopicData layout ───────────────────────────
// Struct sizes and field offsets verified from Cemu OS/libs/nn_olv source.
// DownloadedDataBase (base of DownloadedPostData) field offsets:
static constexpr uint32_t k_PostDataSize  = 0xC208; // sizeof DownloadedPostData (from Cemu)
static constexpr uint32_t k_TopicDataSize = 0x1000; // sizeof DownloadedTopicData (from Cemu)

// ── Module state ──────────────────────────────────────────────────────────────

static OSDynLoad_Module s_handle    = nullptr;
static bool             s_available = false;

// nn::olv::DownloadPostDataList — native post fetch via system HTTP stack
using FnDownloadPosts    = int32_t (*)(void *, void *, uint32_t *, uint32_t, const DownloadParam *);
using FnCtor             = void    (*)(void *);
using FnDLSetUi          = int32_t (*)(void *, uint32_t);
using FnDLSetSearchKey   = int32_t (*)(void *, const uint16_t *, uint8_t);
using FnGetBodyText      = int32_t (*)(const void *, uint16_t *, uint32_t);  // GetBodyText(wchar_t*, u32)
using FnGetMiiNickname   = const uint16_t * (*)(const void *);               // GetMiiNickname() → const wchar_t*
using FnGetFeeling       = int8_t  (*)(const void *);                        // GetFeeling() → s8
using FnGetAppData       = int32_t (*)(const void *, uint8_t *, uint32_t *, uint32_t); // GetAppData(buf,&sz,maxSz)
static FnDownloadPosts   s_fn_download         = nullptr;
static FnCtor            s_fn_ctor_post        = nullptr;
static FnCtor            s_fn_ctor_topic       = nullptr;
static FnDLSetUi         s_fn_dl_set_community = nullptr;
static FnDLSetUi         s_fn_dl_set_max_num   = nullptr;
static FnDLSetSearchKey  s_fn_dl_set_search    = nullptr;
static FnGetBodyText     s_fn_get_body_text    = nullptr;
static FnGetMiiNickname  s_fn_get_mii_nickname = nullptr;
static FnGetFeeling      s_fn_get_feeling      = nullptr;
static FnGetAppData      s_fn_get_appdata      = nullptr;

using FnGetPostId     = const char * (*)(const void *);
static FnGetPostId    s_fn_get_post_id         = nullptr;

// nn::olv::StartPortalApp — launch Miiverse portal at our community / a specific post.
using FnPortalSetUi   = int32_t (*)(void *, uint32_t);
using FnPortalSetStr  = int32_t (*)(void *, const char *);
using FnStartPortal   = int32_t (*)(const void *);
static FnPortalSetUi  s_fn_portal_set_community = nullptr;
static FnPortalSetStr s_fn_portal_set_post_id   = nullptr;
static FnStartPortal  s_fn_start_portal         = nullptr;

// nn::olv::UploadPostDataByPostApp — interactive post creation applet
using FnSetWork        = void    (*)(void *, uint8_t *, uint32_t);
using FnSetBodyText    = void    (*)(void *, const uint16_t *);  // wchar_t* = uint16_t* on Wii U
using FnSetFlags       = void    (*)(void *, uint32_t);
using FnSetTopicTag    = void    (*)(void *, const uint16_t *);  // UTF-16 topic label
using FnSetSearchKey   = void    (*)(void *, const uint16_t *, uint8_t);  // UTF-16 search key + index
using FnSetCommunityId = void    (*)(void *, uint32_t);
using FnSetAppData     = int32_t (*)(void *, const uint8_t *, uint32_t);  // SetAppData(buf, size)
using FnUploadPost     = int32_t (*)(const void *);
static FnCtor          s_fn_ctor_upload    = nullptr;
static FnSetWork       s_fn_set_work       = nullptr;
static FnSetBodyText   s_fn_set_body       = nullptr;
static FnSetFlags      s_fn_set_flags      = nullptr;
static FnSetTopicTag   s_fn_set_topic      = nullptr;
static FnSetSearchKey  s_fn_set_search     = nullptr;
static FnSetCommunityId s_fn_set_community = nullptr;
static FnSetAppData    s_fn_set_appdata    = nullptr;
static FnUploadPost    s_fn_upload_post    = nullptr;

// Binary app-data payload embedded in every post we create (16 bytes, well under 1 KB).
// Lets readers anchor posts to their track timestamp.
struct __attribute__((packed)) PostMeta {
    char     magic[4];    // "SWIU"
    uint8_t  version;     // 1
    uint8_t  _pad[3];
    uint32_t position_ms;
    uint32_t duration_ms;
};
static_assert(sizeof(PostMeta) == 16, "PostMeta must be 16 bytes");

// ── Helpers ───────────────────────────────────────────────────────────────────

// Safely copy a const char* returned by a dynamically resolved nn_olv function.
// Rejects null, out-of-range addresses, non-printable bytes, and strings longer than
// max_len — all failure modes a bad mangled name could produce via a wrong callee.
// Wii U usable memory: MEM1 0x00800000–0x01FFFFFF, MEM2 0x10000000–0x4FFFFFFF.
static std::string safe_cstr(const char *ptr, size_t max_len = 64) {
    if (!ptr) return {};
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    bool in_mem1 = (addr >= 0x00800000u && addr <= 0x01FFFFFFu);
    bool in_mem2 = (addr >= 0x10000000u && addr <= 0x4FFFFFFFu);
    if (!in_mem1 && !in_mem2) return {};
    std::string out;
    out.reserve(max_len);
    for (size_t i = 0; i < max_len; ++i) {
        char c = ptr[i];
        if (c == '\0') break;
        if (c < 0x20 || c > 0x7E) return {};  // non-printable byte → garbage pointer
        out += c;
    }
    return out;
}

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
    // Load nn_olv.rpl.
    // Inkay-Roséverse intercepts AcquireIndependentServiceToken internally during
    // nn_olv::Initialize — no manual AIST pre-call needed.
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
        "SetCommunityId__Q3_2nn3olv25DownloadPostDataListParamFUi",
        reinterpret_cast<void **>(&s_fn_dl_set_community));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "SetPostDataMaxNum__Q3_2nn3olv25DownloadPostDataListParamFUi",
        reinterpret_cast<void **>(&s_fn_dl_set_max_num));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "SetSearchKey__Q3_2nn3olv25DownloadPostDataListParamFPCwUc",
        reinterpret_cast<void **>(&s_fn_dl_set_search));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "GetBodyText__Q3_2nn3olv18DownloadedDataBaseCFPwUi",
        reinterpret_cast<void **>(&s_fn_get_body_text));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "GetMiiNickname__Q3_2nn3olv18DownloadedDataBaseCFv",
        reinterpret_cast<void **>(&s_fn_get_mii_nickname));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "GetFeeling__Q3_2nn3olv18DownloadedDataBaseCFv",
        reinterpret_cast<void **>(&s_fn_get_feeling));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "GetAppData__Q3_2nn3olv18DownloadedDataBaseCFPUcPUiUi",
        reinterpret_cast<void **>(&s_fn_get_appdata));

    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "GetPostId__Q3_2nn3olv18DownloadedPostDataCFv",
        reinterpret_cast<void **>(&s_fn_get_post_id));

    // StartPortalApp — open Miiverse portal at our community / a specific post.
    // Note: the constructor symbol (__ct__...StartPortalAppParamFv) resolves but calls into
    // networking code that hangs before connect and crashes after; zero-fill is sufficient.
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "SetCommunityId__Q3_2nn3olv19StartPortalAppParamFUi",
        reinterpret_cast<void **>(&s_fn_portal_set_community));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "SetPostId__Q3_2nn3olv19StartPortalAppParamFPCc",
        reinterpret_cast<void **>(&s_fn_portal_set_post_id));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "StartPortalApp__Q2_2nn3olvFPCQ3_2nn3olv19StartPortalAppParam",
        reinterpret_cast<void **>(&s_fn_start_portal));
    WHBLogPrintf("olv: dl community=%s maxnum=%s search=%s body=%s nickname=%s feeling=%s appdata=%s",
                 s_fn_dl_set_community    ? "ok" : "missing",
                 s_fn_dl_set_max_num      ? "ok" : "missing",
                 s_fn_dl_set_search       ? "ok" : "missing",
                 s_fn_get_body_text       ? "ok" : "missing",
                 s_fn_get_mii_nickname    ? "ok" : "missing",
                 s_fn_get_feeling         ? "ok" : "missing",
                 s_fn_get_appdata         ? "ok" : "missing");
    WHBLogPrintf("olv: portal community=%s postid=%s start=%s getpostid=%s",
                 s_fn_portal_set_community ? "ok" : "missing",
                 s_fn_portal_set_post_id   ? "ok" : "missing",
                 s_fn_start_portal         ? "ok" : "missing",
                 s_fn_get_post_id          ? "ok" : "missing");

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
        "SetAppData__Q3_2nn3olv15UploadParamBaseFPCUcUi",
        reinterpret_cast<void **>(&s_fn_set_appdata));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "UploadPostDataByPostApp__Q2_2nn3olvFPCQ3_2nn3olv28UploadPostDataByPostAppParam",
        reinterpret_cast<void **>(&s_fn_upload_post));
    WHBLogPrintf("olv: post applet ctor=%s work=%s body=%s flags=%s topic=%s search=%s community=%s appdata=%s upload=%s",
                 s_fn_ctor_upload    ? "ok" : "missing",
                 s_fn_set_work       ? "ok" : "missing",
                 s_fn_set_body       ? "ok" : "missing",
                 s_fn_set_flags      ? "ok" : "missing",
                 s_fn_set_topic      ? "ok" : "missing",
                 s_fn_set_search     ? "ok" : "missing",
                 s_fn_set_community  ? "ok" : "missing",
                 s_fn_set_appdata    ? "ok" : "missing",
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
            if (s_fn_dl_set_community) s_fn_dl_set_community(&param, community_id);
            if (s_fn_dl_set_max_num)   s_fn_dl_set_max_num(&param, limit);
            if (!search_key.empty() && s_fn_dl_set_search) {
                auto sk16 = utf8_to_utf16(search_key, 64);
                s_fn_dl_set_search(&param, sk16.data(), 0);
            }

            uint32_t num_out = 0;
            int32_t rc = s_fn_download(topic.get(), posts.get(), &num_out, limit, &param);
            WHBLogPrintf("olv: DownloadPostDataList → 0x%08X, got %u", (uint32_t)rc, num_out);

            if ((rc == 0 || (uint32_t)rc == 0x01100080u) && num_out > 0) {
                std::vector<Post> out;
                for (uint32_t i = 0; i < num_out; ++i) {
                    const void *p = posts.get() + i * k_PostDataSize;
                    Post post;
                    if (s_fn_get_body_text) {
                        uint16_t buf[201] = {};
                        s_fn_get_body_text(p, buf, 200);
                        post.body = utf16_to_utf8(buf, 200);
                    }
                    if (post.body.empty()) continue;
                    if (s_fn_get_post_id)
                        post.post_id = safe_cstr(s_fn_get_post_id(p));
                    if (s_fn_get_mii_nickname) {
                        const uint16_t *nick = s_fn_get_mii_nickname(p);
                        if (nick) post.screen_name = utf16_to_utf8(nick, 16);
                    }
                    post.feeling = s_fn_get_feeling
                        ? std::max(0, std::min(5, (int)s_fn_get_feeling(p)))
                        : 0;
                    if (s_fn_get_appdata) {
                        PostMeta meta = {};
                        uint32_t sz = 0;
                        int32_t r = s_fn_get_appdata(p, reinterpret_cast<uint8_t *>(&meta),
                                                      &sz, sizeof(meta));
                        if ((r == 0 || (uint32_t)r == 0x01100080u)
                            && sz >= sizeof(meta)
                            && meta.magic[0] == 'S' && meta.magic[1] == 'W'
                            && meta.magic[2] == 'I' && meta.magic[3] == 'U'
                            && meta.version == 1) {
                            post.position_ms = meta.position_ms;
                            post.duration_ms = meta.duration_ms;
                        }
                    }
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
                      const std::string &title, const std::string &search_key,
                      uint32_t position_ms, uint32_t duration_ms) {
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

    // Embed track-timestamp metadata so clients can anchor the post to a position.
    if (s_fn_set_appdata) {
        PostMeta meta = {};
        meta.magic[0] = 'S'; meta.magic[1] = 'W';
        meta.magic[2] = 'I'; meta.magic[3] = 'U';
        meta.version     = 1;
        meta.position_ms = position_ms;
        meta.duration_ms = duration_ms;
        s_fn_set_appdata(s_upload_param, reinterpret_cast<const uint8_t *>(&meta), sizeof(meta));
    }

    int32_t rc = s_fn_upload_post(s_upload_param);
    WHBLogPrintf("olv: UploadPostDataByPostApp → 0x%08X", (uint32_t)rc);
}

void open_overlay(const std::string &post_id) {
    if (s_fn_portal_set_community && s_fn_start_portal) {
        // StartPortalAppParam — plain data struct; zero-fill is equivalent to default-init.
        // The constructor symbol resolves to a wrong function (causes hang/crash), so skip it.
        alignas(32) uint8_t param[0x200] = {};
        WHBLogPrint("olv: portal set_community...");
        s_fn_portal_set_community(param, COMMUNITY_ID);
        if (!post_id.empty() && s_fn_portal_set_post_id) {
            WHBLogPrintf("olv: portal set_post_id=%s", post_id.c_str());
            s_fn_portal_set_post_id(param, post_id.c_str());
        }
        WHBLogPrint("olv: portal start...");
        s_fn_start_portal(param);
    } else {
        SYSSwitchTo(SYSAPP_PFID_MIIVERSE);
    }
}

} // namespace OLV
