#include "olv.h"

#include <cstring>
#include <algorithm>
#include <string>

// stb_image declarations only — implementation lives in display.cpp.
#define STBI_NO_THREAD_LOCALS
#include <stb_image.h>

#include <wut.h>
#include <coreinit/dynload.h>
#include <sysapp/switch.h>
#include <whb/log.h>


#include <nn/ac.h>
#include <nn/act.h>
#include <nsysnet/nssl.h>
#include <coreinit/cache.h>
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
using FnDLSetUc          = int32_t (*)(void *, uint8_t);
using FnDLSetSearchKey   = int32_t (*)(void *, const uint16_t *, uint8_t);
using FnGetBodyText      = int32_t (*)(const void *, uint16_t *, uint32_t);  // GetBodyText(wchar_t*, u32)
using FnGetMiiNickname   = const uint16_t * (*)(const void *);               // GetMiiNickname() → const wchar_t*
using FnGetFeeling       = int8_t  (*)(const void *);                        // GetFeeling() → s8
using FnGetAppData       = int32_t (*)(const void *, uint8_t *, uint32_t *, uint32_t); // GetAppData(buf,&sz,maxSz)
using FnGetMemo          = int32_t (*)(const void *, uint8_t *, uint32_t *, uint32_t); // GetBodyMemo(buf,&sz,maxSz)
static FnDownloadPosts   s_fn_download         = nullptr;
static FnCtor            s_fn_ctor_post        = nullptr;
static FnCtor            s_fn_ctor_topic       = nullptr;
static FnDLSetUi         s_fn_dl_set_community = nullptr;
static FnDLSetUi         s_fn_dl_set_max_num   = nullptr;
static FnDLSetUc         s_fn_dl_set_language  = nullptr;
static FnDLSetSearchKey  s_fn_dl_set_search    = nullptr;
static FnGetBodyText     s_fn_get_body_text    = nullptr;
static FnGetMiiNickname  s_fn_get_mii_nickname = nullptr;
static FnGetFeeling      s_fn_get_feeling      = nullptr;
static FnGetAppData      s_fn_get_appdata      = nullptr;
static FnGetMemo         s_fn_get_memo         = nullptr;

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
using FnAddStampData   = int32_t (*)(void *, const uint8_t *, uint32_t);  // AddStampData(buf, size)
using FnUploadPost     = int32_t (*)(const void *);
static FnCtor          s_fn_ctor_upload    = nullptr;
static FnSetWork       s_fn_set_work       = nullptr;
static FnSetBodyText   s_fn_set_body       = nullptr;
static FnSetFlags      s_fn_set_flags      = nullptr;
static FnSetTopicTag   s_fn_set_topic      = nullptr;
static FnSetSearchKey  s_fn_set_search     = nullptr;
static FnSetCommunityId s_fn_set_community = nullptr;
static FnSetAppData    s_fn_set_appdata    = nullptr;
static FnAddStampData  s_fn_add_stamp      = nullptr;
static FnUploadPost    s_fn_upload_post    = nullptr;

// Pre-encoded stamp TGAs loaded from /vol/content/stamps/ at init time.
// Stored in 32-byte aligned static buffers — Wii U DMA requires this alignment.
static constexpr int   k_MaxStamps    = 100; // SDK STAMP_DATA_MAX_NUM
static constexpr int   k_StampTgaSize = 18 + 100 * 100 * 4 + 26;      // 40044 = STAMP_DATA_100_100_MAX_SIZE
static constexpr int   k_StampBufSize = (k_StampTgaSize + 31) & ~31;  // round up to 32-byte multiple = 40064
alignas(32) static uint8_t s_stamp_bufs[k_MaxStamps][k_StampBufSize];
static int s_stamp_count = 0;

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

// Scale src_w × src_h RGBA pixels to 100×100 and encode as a TGA 2.0 (32-bit BGRA, bottom-left).
// AddStampData validates the TGA 2.0 footer ("TRUEVISION-XFILE.\0"), so the footer is required.
// Total size: 18 (header) + 40000 (pixels) + 26 (footer) = 40044 = STAMP_DATA_100_100_MAX_SIZE.
static std::vector<uint8_t> make_stamp_tga(const uint8_t *src, int src_w, int src_h) {
    if (!src || src_w <= 0 || src_h <= 0) return {};
    constexpr int SW = 100, SH = 100, HDR = 18, FOOTER = 26;
    std::vector<uint8_t> tga(HDR + SW * SH * 4 + FOOTER, 0);
    // Header
    tga[2]  = 2;                        // image type: uncompressed true-color
    tga[12] = SW & 0xFF; tga[13] = 0;  // width  (LE per TGA spec: 0x64 0x00)
    tga[14] = SH & 0xFF; tga[15] = 0;  // height (LE per TGA spec: 0x64 0x00)
    tga[16] = 32;                       // bits per pixel
    tga[17] = 0x08;                     // descriptor: bottom-left, 8 alpha bits
    // Pixels (RGBA → BGRA, converted to grayscale per Roséverse dev requirement)
    // TGA bottom-left origin: row 0 in file = bottom row of image.
    // stbi returns top-row-first, so write in reverse Y order.
    uint8_t *dst = tga.data() + HDR;
    for (int y = 0; y < SH; ++y) {
        int sy = (src_h - 1) - y * src_h / SH;
        for (int x = 0; x < SW; ++x) {
            int sx = x * src_w / SW;
            const uint8_t *s = src + (sy * src_w + sx) * 4;
            // Threshold to black/transparent. nn_olv only accepts three pixel values:
            //   black (0,0,0,255), white (255,255,255,255), transparent (255,255,255,0).
            // Light pixels → transparent (255,255,255,0); dark → opaque black (0,0,0,255).
            bool isLight = ((s[0] * 77 + s[1] * 150 + s[2] * 29) >> 8) > 127;
            dst[0] = isLight ? 255 : 0;
            dst[1] = isLight ? 255 : 0;
            dst[2] = isLight ? 255 : 0;
            dst[3] = isLight ? 0   : 255;
            dst += 4;
        }
    }
    // TGA 2.0 footer (required by AddStampData format validation)
    uint8_t *ftr = tga.data() + HDR + SW * SH * 4;
    // bytes 0-3: extension area offset (0 = none)
    // bytes 4-7: developer directory offset (0 = none)
    static const char sig[] = "TRUEVISION-XFILE.";  // 17 chars + null = 18 bytes
    memcpy(ftr + 8, sig, 18);
    return tga;
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
        "SetLanguageId__Q3_2nn3olv25DownloadPostDataListParamFUc",
        reinterpret_cast<void **>(&s_fn_dl_set_language));
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
        "GetBodyMemo__Q3_2nn3olv18DownloadedDataBaseCFPUcPUiUi",
        reinterpret_cast<void **>(&s_fn_get_memo));

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
    WHBLogPrintf("olv: dl community=%s maxnum=%s search=%s body=%s nickname=%s feeling=%s appdata=%s memo=%s",
                 s_fn_dl_set_community    ? "ok" : "missing",
                 s_fn_dl_set_max_num      ? "ok" : "missing",
                 s_fn_dl_set_search       ? "ok" : "missing",
                 s_fn_get_body_text       ? "ok" : "missing",
                 s_fn_get_mii_nickname    ? "ok" : "missing",
                 s_fn_get_feeling         ? "ok" : "missing",
                 s_fn_get_appdata         ? "ok" : "missing",
                 s_fn_get_memo            ? "ok" : "missing");
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
        "AddStampData__Q3_2nn3olv28UploadPostDataByPostAppParamFPCUcUi",
        reinterpret_cast<void **>(&s_fn_add_stamp));
    OSDynLoad_FindExport(s_handle, OS_DYNLOAD_EXPORT_FUNC,
        "UploadPostDataByPostApp__Q2_2nn3olvFPCQ3_2nn3olv28UploadPostDataByPostAppParam",
        reinterpret_cast<void **>(&s_fn_upload_post));
    WHBLogPrintf("olv: post applet ctor=%s work=%s body=%s flags=%s topic=%s search=%s community=%s appdata=%s stamp=%s upload=%s",
                 s_fn_ctor_upload    ? "ok" : "missing",
                 s_fn_set_work       ? "ok" : "missing",
                 s_fn_set_body       ? "ok" : "missing",
                 s_fn_set_flags      ? "ok" : "missing",
                 s_fn_set_topic      ? "ok" : "missing",
                 s_fn_set_search     ? "ok" : "missing",
                 s_fn_set_community  ? "ok" : "missing",
                 s_fn_set_appdata    ? "ok" : "missing",
                 s_fn_add_stamp      ? "ok" : "missing",
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
                // Load stamps from /vol/content/stamps/stamp1.png upward;
                // stop at the first missing file (sequential naming assumed).
                s_stamp_count = 0;
                for (int i = 1; i <= k_MaxStamps; ++i) {
                    char path[64];
                    snprintf(path, sizeof(path), "/vol/content/stamps/stamp%d.png", i);
                    FILE *fp = fopen(path, "rb");
                    if (!fp) break;
                    fseek(fp, 0, SEEK_END); long sz = ftell(fp); rewind(fp);
                    std::vector<uint8_t> raw(sz);
                    fread(raw.data(), 1, sz, fp); fclose(fp);
                    int w = 0, h = 0, ch = 0;
                    uint8_t *rgba = stbi_load_from_memory(raw.data(), (int)raw.size(), &w, &h, &ch, 4);
                    if (!rgba) { WHBLogPrintf("olv: stamp %d decode failed", i); continue; }
                    auto tga = make_stamp_tga(rgba, w, h);
                    stbi_image_free(rgba);
                    if (!tga.empty()) {
                        memcpy(s_stamp_bufs[s_stamp_count], tga.data(), tga.size());
                        WHBLogPrintf("olv: loaded stamp %d (%dx%d) → buf[%d]",
                                     i, w, h, s_stamp_count);
                        ++s_stamp_count;
                    }
                }
                WHBLogPrintf("olv: %d stamps loaded", s_stamp_count);

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
            if (s_fn_dl_set_language)  s_fn_dl_set_language(&param, 254);  // 254 = all languages
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
                    // TGA 320×120 BGRA from lower-left; max compressed → ~40 KB in work buf,
                    // decompressed to 18 + 320*120*4 = 153,618 bytes in the output buffer.
                    if (s_fn_get_memo) {
                        static uint8_t s_memo_buf[153644];
                        uint32_t memo_sz = 0;
                        int32_t mr = s_fn_get_memo(p, s_memo_buf, &memo_sz, sizeof(s_memo_buf));
                        if ((mr == 0 || (uint32_t)mr == 0x01100080u) && memo_sz >= 18)
                            post.memo.assign(s_memo_buf, s_memo_buf + memo_sz);
                    }
                    if (post.body.empty() && post.memo.empty()) continue;
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

    // UploadPostDataByPostAppParam true size (from Cafe SDK):
    //   UploadParamBase: vtable(4) + bodyText[256](512) + topicTag[152](304)
    //     + searchKeys[5][152](1520) + scalars(~100) ≈ 2440 bytes
    //   UploadPostDataParam adds: reserved[2712]  → +2712
    //   UploadPostDataByPostAppParam adds: reserved[508] → +508
    //   Total ≈ 5660 bytes — use 0x2000 (8192) so the constructor never
    //   writes past our buffer into adjacent BSS.
    static constexpr uint32_t k_UploadParamSize = 0x2000;
    alignas(32) static uint8_t s_upload_work[0x100000];  // 1 MB — POST_APP_PARAM_WORK_BUFF_SIZE
    alignas(32) static uint8_t s_upload_param[k_UploadParamSize];

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

    // Add pre-loaded stamps so users can place them on their drawings.
    if (s_fn_add_stamp && s_stamp_count > 0) {
        int added = 0;
        for (int i = 0; i < s_stamp_count; ++i) {
            DCFlushRange(s_stamp_bufs[i], k_StampTgaSize);
            int32_t sr = s_fn_add_stamp(s_upload_param, s_stamp_bufs[i], k_StampTgaSize);
            if (sr == 0 || (uint32_t)sr == 0x01100080u) ++added;
            else WHBLogPrintf("olv: AddStampData[%d] → 0x%08X", i, (uint32_t)sr);
        }
        WHBLogPrintf("olv: %d/%d stamps added", added, s_stamp_count);
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
