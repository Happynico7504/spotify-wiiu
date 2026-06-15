#pragma once
#include <string>
#include <vector>
#include <functional>

namespace Connect {

// Spotify Device Authorization flow.
// Calls Spotify's /oauth2/device/authorize endpoint, fetches a QR code PNG,
// then polls /api/token until the user authorizes or the code expires.
//
// on_login(user_code, qr_png) — called once with display data (from background thread)
// on_token(access_token)      — called when the user completes auth
// on_error(msg)               — called on network failure or expiry
//
// Runs synchronously; call from a dedicated thread.
void device_auth(
    std::function<void(std::string user_code, std::vector<uint8_t> qr_png)> on_login,
    std::function<void(std::string access_token)>                           on_token,
    std::function<void(std::string msg)>                                    on_error,
    std::function<bool()>                                                   is_running);

} // namespace Connect
