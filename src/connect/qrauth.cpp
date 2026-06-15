#include "qrauth.h"

#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <whb/log.h>
#include "cJSON/cJSON.h"

static constexpr char DEVICE_AUTH_URL[] =
    "https://accounts.spotify.com/oauth2/device/authorize";
static constexpr char TOKEN_URL[] =
    "https://accounts.spotify.com/api/token";
static constexpr char CLIENT_ID[] =
    "65b708073fc0480ea92a077233ca87bd";

// ── curl helpers ──────────────────────────────────────────────────────────────

static size_t curl_append(char *ptr, size_t sz, size_t n, void *ud) {
    auto *buf = static_cast<std::vector<uint8_t> *>(ud);
    buf->insert(buf->end(), ptr, ptr + sz * n);
    return sz * n;
}

static std::vector<uint8_t> http_post(const char *url, const char *body) {
    std::vector<uint8_t> out;
    CURL *c = curl_easy_init();
    if (!c) return out;
    curl_easy_setopt(c, CURLOPT_URL,            url);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,     body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  curl_append);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
    return out;
}

static std::vector<uint8_t> http_get(const char *url) {
    std::vector<uint8_t> out;
    CURL *c = curl_easy_init();
    if (!c) return out;
    curl_easy_setopt(c, CURLOPT_URL,            url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  curl_append);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
    return out;
}

static std::string urlencode(const std::string &s) {
    CURL *c = curl_easy_init();
    if (!c) return s;
    char *enc = curl_easy_escape(c, s.c_str(), (int)s.size());
    std::string result = enc ? enc : s;
    curl_free(enc);
    curl_easy_cleanup(c);
    return result;
}

static std::string vec_to_str(const std::vector<uint8_t> &v) {
    return std::string(reinterpret_cast<const char *>(v.data()), v.size());
}

// ── device_auth ───────────────────────────────────────────────────────────────

void Connect::device_auth(
    std::function<void(std::string, std::vector<uint8_t>)> on_login,
    std::function<void(std::string)>                       on_token,
    std::function<void(std::string)>                       on_error,
    std::function<bool()>                                  is_running)
{
    // Step 1 — request device code
    std::string req = std::string("client_id=") + CLIENT_ID + "&scope=streaming";
    auto resp_bytes = http_post(DEVICE_AUTH_URL, req.c_str());
    std::string resp = vec_to_str(resp_bytes);
    WHBLogPrintf("qrauth: device/authorize resp len=%zu", resp.size());

    // Log safe ASCII portion of response for diagnosis.
    { std::string dbg = resp; for (char &c : dbg) if ((unsigned char)c < 32) c = '?';
      WHBLogPrintf("qrauth: resp: %.250s", dbg.c_str()); }

    cJSON *root = cJSON_Parse(resp.c_str());
    if (!root) {
        on_error("Spotify login failed (network error)");
        return;
    }

    // Surface Spotify error if present.
    const char *err_val = cJSON_GetStringValue(cJSON_GetObjectItem(root, "error"));
    const char *err_desc = cJSON_GetStringValue(cJSON_GetObjectItem(root, "error_description"));
    if (err_val) {
        WHBLogPrintf("qrauth: Spotify error: %s — %s", err_val, err_desc ? err_desc : "");
        std::string msg = std::string("Spotify: ") + err_val;
        if (err_desc) msg += std::string(" (") + err_desc + ")";
        cJSON_Delete(root);
        on_error(msg);
        return;
    }

    const char *device_code = cJSON_GetStringValue(cJSON_GetObjectItem(root, "device_code"));
    const char *user_code   = cJSON_GetStringValue(cJSON_GetObjectItem(root, "user_code"));
    const char *verify_uri  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "verification_uri_complete"));
    cJSON      *iv_item     = cJSON_GetObjectItem(root, "interval");
    cJSON      *ex_item     = cJSON_GetObjectItem(root, "expires_in");

    int poll_interval = iv_item ? (int)cJSON_GetNumberValue(iv_item) : 5;
    int expires_in    = ex_item ? (int)cJSON_GetNumberValue(ex_item) : 300;

    if (!device_code || !user_code || !verify_uri) {
        WHBLogPrint("qrauth: missing fields in device/authorize response");
        cJSON_Delete(root);
        on_error("Spotify login failed (bad response)");
        return;
    }

    WHBLogPrintf("qrauth: user_code=%s", user_code);

    // Step 2 — fetch QR PNG
    std::string qr_url = std::string("https://quickchart.io/qr?text=")
                       + urlencode(verify_uri)
                       + "&size=300&margin=1&format=png";
    auto qr_png = http_get(qr_url.c_str());
    WHBLogPrintf("qrauth: QR PNG %zu bytes", qr_png.size());

    on_login(std::string(user_code), std::move(qr_png));

    // Step 3 — poll for token
    std::string poll_body = std::string("client_id=") + CLIENT_ID
        + "&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"
        + "&device_code=" + urlencode(std::string(device_code));

    cJSON_Delete(root);

    for (int elapsed = 0; is_running() && elapsed < expires_in; elapsed += poll_interval) {
        // Sleep in 1-second increments so is_running() is checked often.
        for (int i = 0; i < poll_interval && is_running(); ++i) {
            struct timespec ts = {1, 0};
            nanosleep(&ts, nullptr);
        }
        if (!is_running()) break;

        auto tr_bytes = http_post(TOKEN_URL, poll_body.c_str());
        std::string tr_str = vec_to_str(tr_bytes);

        cJSON *tr = cJSON_Parse(tr_str.c_str());
        if (!tr) continue;

        const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(tr, "error"));
        if (err) {
            if (strcmp(err, "slow_down") == 0)
                poll_interval += 5;
            else if (strcmp(err, "authorization_pending") == 0)
                ; // keep polling
            else {
                WHBLogPrintf("qrauth: poll error: %s", err);
                cJSON_Delete(tr);
                on_error("Spotify login cancelled");
                return;
            }
            cJSON_Delete(tr);
            continue;
        }

        const char *access_token = cJSON_GetStringValue(cJSON_GetObjectItem(tr, "access_token"));
        if (access_token) {
            WHBLogPrint("qrauth: got access token");
            std::string tok(access_token);
            cJSON_Delete(tr);
            on_token(std::move(tok));
            return;
        }
        cJSON_Delete(tr);
    }

    WHBLogPrint("qrauth: timed out");
    on_error("Spotify login timed out — restart the app");
}
