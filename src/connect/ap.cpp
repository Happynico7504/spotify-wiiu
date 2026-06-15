#include "ap.h"
#include "shannon.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <algorithm>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cerrno>
#include <poll.h>
#include <fcntl.h>

#include <mbedtls/bignum.h>
#include <mbedtls/md.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

#include <curl/curl.h>
#include <whb/log.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>

namespace Connect {

// ── DH parameters — must match librespot exactly (96-byte / 768-bit prime) ────
static const uint8_t AP_DH_PRIME[96] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xc9,0x0f,0xda,0xa2,0x21,0x68,0xc2,0x34,
    0xc4,0xc6,0x62,0x8b,0x80,0xdc,0x1c,0xd1,0x29,0x02,0x4e,0x08,0x8a,0x67,0xcc,0x74,
    0x02,0x0b,0xbe,0xa6,0x3b,0x13,0x9b,0x22,0x51,0x4a,0x08,0x79,0x8e,0x34,0x04,0xdd,
    0xef,0x95,0x19,0xb3,0xcd,0x3a,0x43,0x1b,0x30,0x2b,0x0a,0x6d,0xf2,0x5f,0x14,0x37,
    0x4f,0xe1,0x35,0x6d,0x6d,0x51,0xc2,0x45,0xe4,0x85,0xb5,0x76,0x62,0x5e,0x7e,0xc6,
    0xf4,0x4c,0x42,0xe9,0xa6,0x3a,0x36,0x20,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
};
static const uint8_t AP_DH_GEN[1] = { 0x02 };

// ── Protobuf helpers ──────────────────────────────────────────────────────────

static void pb_vi(std::vector<uint8_t> &b, uint64_t v) {
    do {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if (v) byte |= 0x80;
        b.push_back(byte);
    } while (v);
}
static void pb_tag(std::vector<uint8_t> &b, uint32_t f, uint8_t w) {
    pb_vi(b, ((uint64_t)f << 3) | w);
}
static void pb_u32(std::vector<uint8_t> &b, uint32_t f, uint32_t v) {
    pb_tag(b, f, 0); pb_vi(b, v);
}
static void pb_u64(std::vector<uint8_t> &b, uint32_t f, uint64_t v) {
    pb_tag(b, f, 0); pb_vi(b, v);
}
static void pb_bytes(std::vector<uint8_t> &b, uint32_t f, const uint8_t *p, size_t n) {
    pb_tag(b, f, 2); pb_vi(b, n); b.insert(b.end(), p, p + n);
}
static void pb_str(std::vector<uint8_t> &b, uint32_t f, const std::string &s) {
    pb_bytes(b, f, reinterpret_cast<const uint8_t *>(s.data()), s.size());
}
static void pb_msg(std::vector<uint8_t> &b, uint32_t f, const std::vector<uint8_t> &m) {
    pb_bytes(b, f, m.data(), m.size());
}

// ── Protobuf reader ───────────────────────────────────────────────────────────

struct PRd {
    const uint8_t *p, *end;
    bool ok = true;

    bool vi(uint64_t &out) {
        out = 0; int sh = 0;
        while (p < end) {
            uint8_t b = *p++;
            out |= (uint64_t)(b & 0x7F) << sh; sh += 7;
            if (!(b & 0x80)) return true;
        }
        return ok = false;
    }
    bool next(uint32_t &field, uint8_t &wire) {
        if (p >= end) return false;
        uint64_t v; if (!vi(v)) return false;
        field = (uint32_t)(v >> 3); wire = v & 7; return true;
    }
    bool skip(uint8_t w) {
        uint64_t n;
        switch (w) {
            case 0: return vi(n);
            case 1: if (p+8>end){ok=false;return false;} p+=8; return true;
            case 2: if (!vi(n)||p+n>end){ok=false;return false;} p+=n; return true;
            case 5: if (p+4>end){ok=false;return false;} p+=4; return true;
            default: return ok=false;
        }
    }
    bool enter(PRd &sub) {
        uint64_t n; if (!vi(n)||p+n>end) return ok=false;
        sub = {p, p+(size_t)n, true}; p += (size_t)n; return true;
    }
    bool read_bytes(std::vector<uint8_t> &out) {
        uint64_t n; if (!vi(n)||p+n>end) return ok=false;
        out.assign(p, p+(size_t)n); p += (size_t)n; return true;
    }
    bool read_str(std::string &out) {
        uint64_t n; if (!vi(n)||p+n>end) return ok=false;
        out.assign(reinterpret_cast<const char *>(p), (size_t)n);
        p += (size_t)n; return true;
    }
};

// ── HMAC-SHA1 helper ──────────────────────────────────────────────────────────

static void hmac_sha1(const uint8_t *key, size_t klen,
                      const std::initializer_list<std::pair<const uint8_t*,size_t>> &parts,
                      uint8_t out[20])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, md, 1);
    mbedtls_md_hmac_starts(&ctx, key, klen);
    for (auto &[p, n] : parts)
        mbedtls_md_hmac_update(&ctx, p, n);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}

// ── curl write callback (shared) ──────────────────────────────────────────────

static size_t curl_write_str(char *ptr, size_t sz, size_t n, void *ud) {
    static_cast<std::string *>(ud)->append(ptr, sz * n);
    return sz * n;
}

// ── AP constructor / destructor ───────────────────────────────────────────────

AP::AP() = default;

AP::~AP() { disconnect(); }

// ── resolve_ap ────────────────────────────────────────────────────────────────
// Fetches the AP address list from apresolve.spotify.com and returns the first
// entry.  Falls back to gue1-spclient.spotify.com:4070 on any failure.

bool AP::resolve_ap(std::vector<std::pair<std::string,uint16_t>> &aps) {
    // Fallback list — port 443 first (port 4070 may be blocked at ISP/router).
    // Spotify's AP speaks its own binary protocol on all ports — NOT TLS.
    aps = {
        {"ap-gue1.spotify.com", 4070},
        {"ap-gue1.spotify.com", 443},
        {"ap-gew4.spotify.com", 443},
        {"ap-gew4.spotify.com", 4070},
    };

    CURL *curl = curl_easy_init();
    if (!curl) return true;

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, "https://apresolve.spotify.com/?type=accesspoint");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_str);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Spotify/8.6.0 iOS/14.0 (iPhone9,3)");
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        WHBLogPrintf("ap: apresolve failed (%d), using fallback", rc);
        return true;
    }
    WHBLogPrintf("ap: apresolve: %.400s", body.c_str());

    // Parse all entries from "accesspoint":[...]
    auto pos = body.find("\"accesspoint\"");
    if (pos == std::string::npos) return true;
    pos = body.find('[', pos);
    if (pos == std::string::npos) return true;
    auto arr_end = body.find(']', pos);
    if (arr_end == std::string::npos) return true;

    std::vector<std::pair<std::string,uint16_t>> parsed;
    while (pos < arr_end) {
        pos = body.find('"', pos);
        if (pos == std::string::npos || pos >= arr_end) break;
        ++pos;
        auto end = body.find('"', pos);
        if (end == std::string::npos || end > arr_end) break;
        std::string entry = body.substr(pos, end - pos);
        pos = end + 1;
        auto colon = entry.rfind(':');
        if (colon != std::string::npos) {
            std::string h = entry.substr(0, colon);
            uint16_t p = (uint16_t)std::stoul(entry.substr(colon + 1));
            parsed.push_back({h, p});
        }
    }

    if (!parsed.empty()) {
        // gue1 (US) always replies immediately; gew4 (Germany) consistently
        // ignores our Login on this network.  gue1 only listens on port 4070
        // (port 443 gives RST), so put gue1:4070 first, then the apresolve
        // list (443 before 4070 since 4070 may be blocked on some networks).
        std::vector<std::pair<std::string,uint16_t>> sorted;
        sorted.push_back({"ap-gue1.spotify.com", 4070});
        for (auto &e : parsed) if (e.second == 443)  sorted.push_back(e);
        for (auto &e : parsed) if (e.second == 4070 && e.first != "ap-gue1.spotify.com") sorted.push_back(e);
        aps = std::move(sorted);
    }
    return true;
}

// ── tcp_connect ───────────────────────────────────────────────────────────────

bool AP::tcp_connect(const std::string &h, uint16_t p) {
    if (p == 80) {
        WHBLogPrintf("ap: skipping %s:80 (transparent proxy risk)", h.c_str());
        return false;
    }
    // Use http:// so curl makes a plain TCP connection with no TLS.
    // Spotify's AP speaks its own binary protocol on all ports (443, 4070) —
    // NOT HTTP/TLS. We use curl because POSIX recv() is permanently broken for
    // outbound TCP on Wii U; curl_easy_send/recv work correctly.
    std::string url = std::string("http://") + h + ":" + std::to_string(p);
    CURL *curl = curl_easy_init();
    if (!curl) { WHBLogPrint("ap: curl_easy_init failed"); return false; }

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY,   1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    WHBLogPrintf("ap: trying %s:%u via curl (raw TCP)", h.c_str(), p);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        WHBLogPrintf("ap: curl TCP to %s:%u failed: %s (%d)",
                     h.c_str(), p, curl_easy_strerror(rc), (int)rc);
        curl_easy_cleanup(curl);
        return false;
    }
    curl_socket_t sock = CURL_SOCKET_BAD;
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);
    fd_   = (int)sock;
    curl_ = curl;

    // Disable Nagle — we send small framed packets and need them delivered
    // immediately.  Without TCP_NODELAY the 4-byte Shannon MAC can be held
    // in the kernel send buffer indefinitely, stalling the server response.
    int one = 1;
    int nd = setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    WHBLogPrintf("ap: TCP connected to %s:%u (fd=%d TCP_NODELAY=%d)",
                 h.c_str(), p, (int)sock, nd);
    return true;
}

// ── send_raw / recv_exact ─────────────────────────────────────────────────────

bool AP::send_raw(const uint8_t *buf, size_t len) {
    // After the handshake, use raw POSIX send() — curl_easy_send may silently
    // buffer data without flushing, preventing the server from seeing the Login.
    if (raw_recv_) {
        size_t total_sent = 0;
        while (total_sent < len) {
            ssize_t n = ::send(fd_, buf + total_sent, len - total_sent, 0);
            if (n < 0) {
                WHBLogPrintf("ap: raw send failed errno=%d total=%zu want=%zu",
                             errno, total_sent, len);
                return false;
            }
            total_sent += (size_t)n;
        }
        return true;
    }
    if (curl_) {
        size_t total_sent = 0;
        int again = 0;
        while (len > 0) {
            size_t sent = 0;
            CURLcode rc = curl_easy_send(curl_, buf, len, &sent);
            if (rc == CURLE_AGAIN) {
                ++again;
                if (again >= 500) return false;
                OSSleepTicks(OSMillisecondsToTicks(2));
                continue;
            }
            if (rc != CURLE_OK || sent == 0) {
                WHBLogPrintf("ap: curl_easy_send failed rc=%d sent=%zu want=%zu total=%zu",
                             (int)rc, sent, len, total_sent);
                return false;
            }
            again = 0;
            buf += sent; len -= sent; total_sent += sent;
        }
        return true;
    }
    while (len > 0) {
        int n = (int)::send(fd_, buf, len, 0);
        if (n <= 0) return false;
        buf += n; len -= (size_t)n;
    }
    return true;
}

bool AP::recv_exact(uint8_t *buf, size_t n) {
    // After the handshake, use blocking POSIX recv() — curl's internal state
    // appears to absorb incoming data without signalling POLLIN on the fd.
    if (raw_recv_) {
        int timeouts = 0;
        while (n > 0) {
            if (stop_.load()) return false;
            ssize_t r = ::recv(fd_, buf, n, 0);  // blocking; SO_RCVTIMEO gives 5s slices
            if (r > 0) {
                buf += (size_t)r; n -= (size_t)r; timeouts = 0;
            } else if (r == 0) {
                WHBLogPrint("ap: raw recv: connection closed");
                return false;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                OSSleepTicks(OSMillisecondsToTicks(500));
                if (++timeouts >= 360) { WHBLogPrint("ap: raw recv: 3 min timeout"); return false; }
            } else {
                WHBLogPrintf("ap: raw recv error errno=%d (want %zu)", errno, n);
                return false;
            }
        }
        return true;
    }
    if (curl_) {
        int poll_timeouts = 0;
        while (n > 0) {
            if (stop_.load()) return false;

            size_t got = 0;
            CURLcode rc = curl_easy_recv(curl_, buf, n, &got);
            if (rc == CURLE_OK) {
                if (got == 0) {
                    // Server sent FIN; response bytes may still be in the OS socket buffer.
                    ssize_t r = recv(fd_, buf, n, 0);
                    if (r > 0) { buf += (size_t)r; n -= (size_t)r; poll_timeouts = 0; continue; }
                    return false;
                }
                buf += got; n -= got;
                poll_timeouts = 0;
                continue;
            }
            if (rc != CURLE_AGAIN) {
                WHBLogPrintf("ap: curl_recv error rc=%d", (int)rc);
                return false;
            }

            // CURLE_AGAIN — block on the raw socket fd with poll() instead of
            // busy-sleeping. curl_easy_recv on Wii U keeps returning EAGAIN even
            // when data is present; poll() sees the real socket state.
            if (fd_ < 0) {
                OSSleepTicks(OSMillisecondsToTicks(10));
                continue;
            }

            struct pollfd pfd = { fd_, POLLIN | POLLHUP | POLLERR, 0 };
            int pr = poll(&pfd, 1, 5000);  // block up to 5 s per attempt
            if (pr < 0) {
                WHBLogPrintf("ap: poll error errno=%d", errno);
                return false;
            }
            if (pr == 0) {
                // poll() timed out.  On the Wii U, POLLIN may never fire for data
                // that arrived while the socket was idle — try raw recv() directly in
                // case the bytes are already in the OS buffer.
                {
                    int fl = fcntl(fd_, F_GETFL, 0);
                    if (fl >= 0) fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
                    ssize_t r = ::recv(fd_, buf, n, 0);
                    if (fl >= 0) fcntl(fd_, F_SETFL, fl);
                    if (r > 0) {
                        buf += (size_t)r; n -= (size_t)r;
                        poll_timeouts = 0;
                        continue;
                    }
                }
                ++poll_timeouts;
                if (poll_timeouts >= 36) { WHBLogPrint("ap: recv_exact: timeout (3 min)"); return false; }
                continue;
            }

            if (pfd.revents & (POLLHUP | POLLERR)) {
                WHBLogPrint("ap: recv_exact: connection closed (HUP/ERR)");
                return false;
            }

            if (pfd.revents & POLLIN) {
                // Force O_NONBLOCK FIRST — on Wii U, nsysnet transitions the socket
                // into blocking mode when a FIN or data segment arrives, causing
                // curl_easy_recv and raw recv() to block indefinitely.
                // fcntl works on Wii U nsysnet sockets; ioctl(FIONBIO) does not.
                int saved_flags = fcntl(fd_, F_GETFL, 0);
                if (saved_flags >= 0) fcntl(fd_, F_SETFL, saved_flags | O_NONBLOCK);

                // Try curl a few times quickly (socket now non-blocking).
                bool advanced = false;
                for (int i = 0; i < 5 && !advanced; ++i) {
                    size_t got2 = 0;
                    CURLcode rc2 = curl_easy_recv(curl_, buf, n, &got2);
                    if (rc2 == CURLE_OK && got2 > 0) {
                        buf += got2; n -= got2; poll_timeouts = 0; advanced = true;
                    } else if (rc2 == CURLE_OK && got2 == 0) {
                        if (saved_flags >= 0) fcntl(fd_, F_SETFL, saved_flags);
                        return false;
                    } else if (rc2 != CURLE_AGAIN) {
                        // curl closed its end (FIN received); data may still be in
                        // the OS socket buffer — fall through to raw recv() below.
                        break;
                    }
                }

                if (!advanced) {
                    ssize_t r = recv(fd_, buf, n, 0);
                    if (r > 0) { buf += (size_t)r; n -= (size_t)r; poll_timeouts = 0; advanced = true; }
                    else if (r == 0 || errno == EBADF || errno == ECONNRESET) {
                        if (saved_flags >= 0) fcntl(fd_, F_SETFL, saved_flags);
                        return false;
                    }
                    // errno==EAGAIN: spurious POLLIN — re-poll after brief pause
                    if (!advanced) OSSleepTicks(OSMillisecondsToTicks(50));
                }

                if (saved_flags >= 0) fcntl(fd_, F_SETFL, saved_flags);
            }
        }
        return true;
    }
    while (n > 0) {
        int r = (int)::recv(fd_, buf, n, 0);
        if (r <= 0) return false;
        buf += r; n -= (size_t)r;
    }
    return true;
}

// ── Handshake plain-packet helpers ────────────────────────────────────────────

bool AP::send_plain(const std::vector<uint8_t> &msg) {
    uint32_t total = (uint32_t)(msg.size() + 4);
    uint8_t hdr[4] = { (uint8_t)(total>>24),(uint8_t)(total>>16),
                       (uint8_t)(total>>8), (uint8_t)total };
    return send_raw(hdr, 4) && send_raw(msg.data(), msg.size());
}

bool AP::recv_plain(std::vector<uint8_t> &msg, std::vector<uint8_t> &raw) {
    // APResponseMessage wire format: [u32 BE total][proto]  (total = 4 + proto_len)
    uint8_t hdr[4];
    if (!recv_exact(hdr, 4)) {
        WHBLogPrint("ap: recv_plain: failed to read 4-byte header");
        return false;
    }
    uint32_t total = ((uint32_t)hdr[0]<<24)|((uint32_t)hdr[1]<<16)|
                     ((uint32_t)hdr[2]<<8)|(uint32_t)hdr[3];
    if (total < 4 || total > 65536) {
        WHBLogPrintf("ap: recv_plain: bad total=%u (hdr=%02X%02X%02X%02X)",
                     total, hdr[0], hdr[1], hdr[2], hdr[3]);
        return false;
    }
    msg.resize(total - 4);
    if (!msg.empty() && !recv_exact(msg.data(), msg.size())) {
        WHBLogPrint("ap: recv_plain: failed to read proto body");
        return false;
    }
    raw.insert(raw.end(), hdr, hdr + 4);
    raw.insert(raw.end(), msg.begin(), msg.end());
    return true;
}

// (no custom exp_mod needed — the Spotify prime is odd so mbedtls_mpi_exp_mod works)


// ── do_handshake ──────────────────────────────────────────────────────────────

bool AP::do_handshake() {
    // ── DH key pair ──────────────────────────────────────────────────────────
    WHBLogPrint("ap: handshake: generating DH key...");
    auto *entropy  = new mbedtls_entropy_context;
    auto *ctr_drbg = new mbedtls_ctr_drbg_context;
    mbedtls_entropy_init(entropy);
    mbedtls_ctr_drbg_init(ctr_drbg);
    mbedtls_ctr_drbg_seed(ctr_drbg, mbedtls_entropy_func, entropy,
                          reinterpret_cast<const uint8_t *>("ap"), 2);

    mbedtls_mpi P_mpi, G_mpi, X_mpi, GX_mpi;
    mbedtls_mpi_init(&P_mpi); mbedtls_mpi_init(&G_mpi);
    mbedtls_mpi_init(&X_mpi); mbedtls_mpi_init(&GX_mpi);
    mbedtls_mpi_read_binary(&P_mpi, AP_DH_PRIME, sizeof(AP_DH_PRIME));
    mbedtls_mpi_read_binary(&G_mpi, AP_DH_GEN,   sizeof(AP_DH_GEN));

    // 95 random bytes → private key always < 96-byte prime (leading byte 0xFF)
    mbedtls_mpi_fill_random(&X_mpi, 95, mbedtls_ctr_drbg_random, ctr_drbg);

    int mpi_ret = mbedtls_mpi_exp_mod(&GX_mpi, &G_mpi, &X_mpi, &P_mpi, nullptr);
    mbedtls_mpi_free(&G_mpi);

    // Always write exactly sizeof(AP_DH_PRIME) bytes — mbedtls zero-pads on the
    // left if the result happens to have a leading zero byte.
    static constexpr size_t DH_KEY_SIZE = sizeof(AP_DH_PRIME);
    size_t our_pub_len = DH_KEY_SIZE;
    std::vector<uint8_t> our_pub(DH_KEY_SIZE, 0);
    if (mpi_ret == 0)
        mpi_ret = mbedtls_mpi_write_binary(&GX_mpi, our_pub.data(), DH_KEY_SIZE);
    mbedtls_mpi_free(&GX_mpi);

    WHBLogPrintf("ap: handshake: DH key ready (ret=%d, len=%zu, pub[0..3]=%02X%02X%02X%02X)",
                 mpi_ret, our_pub_len,
                 our_pub[0], our_pub[1], our_pub[2], our_pub[3]);
    if (mpi_ret != 0) {
        WHBLogPrintf("ap: handshake: DH keygen failed (-0x%04X)", -mpi_ret);
        mbedtls_mpi_free(&P_mpi); mbedtls_mpi_free(&X_mpi);
        mbedtls_ctr_drbg_free(ctr_drbg); mbedtls_entropy_free(entropy);
        delete ctr_drbg; delete entropy;
        return false;
    }

    // 16-byte client nonce
    uint8_t client_nonce[16];
    mbedtls_ctr_drbg_random(ctr_drbg, client_nonce, 16);

    mbedtls_ctr_drbg_free(ctr_drbg);
    mbedtls_entropy_free(entropy);
    delete ctr_drbg; delete entropy;

    // ── Build ClientHello ─────────────────────────────────────────────────────
    std::vector<uint8_t> build_info;
    pb_u32(build_info, 10, 0);          // product = PRODUCT_CLIENT
    pb_u32(build_info, 20, 0);          // product_flags = PRODUCT_FLAG_NONE
    pb_u32(build_info, 30, 8);          // platform = PLATFORM_LINUX_X86_64
    pb_u64(build_info, 40, 124200290);  // version

    // LoginCryptoDiffieHellmanHello { gc=our_pub, server_keys_known=1 }
    std::vector<uint8_t> dh_hello;
    pb_bytes(dh_hello, 10, our_pub.data(), our_pub.size());
    pb_u32(dh_hello,  20, 1);

    // LoginCryptoHelloUnion { diffie_hellman=dh_hello }
    std::vector<uint8_t> crypto_hello;
    pb_msg(crypto_hello, 10, dh_hello);

    // ClientHello — field numbers from keyexchange.proto:
    //   10=build_info, 30=cryptosuites_supported, 50=login_crypto_hello,
    //   60=client_nonce, 70=padding  (field 40=powschemes_supported is omitted)
    std::vector<uint8_t> client_hello;
    pb_msg(client_hello,  10, build_info);
    pb_u32(client_hello,  20, 0);           // fingerprints_supported = FINGERPRINT_GRAIN_128
    pb_u32(client_hello,  30, 0);           // cryptosuites_supported = CRYPTO_SUITE_SHANNON
    pb_msg(client_hello,  50, crypto_hello); // login_crypto_hello
    pb_bytes(client_hello, 60, client_nonce, 16); // client_nonce
    uint8_t padding[1] = { 0x1e };
    pb_bytes(client_hello, 70, padding, sizeof(padding)); // padding

    // Send and save raw bytes (incl. header) for HMAC
    // ClientHello wire format: [0x00 0x04][u32 BE total=2+4+proto][proto]
    // The first two bytes are the constant marker 0x00 0x04, NOT a size field.
    std::vector<uint8_t> client_hello_raw;
    {
        size_t   proto_len = client_hello.size();
        uint32_t total     = (uint32_t)(2 + 4 + proto_len);
        uint8_t  hdr[6]    = {
            0x00, 0x04,
            (uint8_t)(total >> 24), (uint8_t)((total >> 16) & 0xFF),
            (uint8_t)((total >>  8) & 0xFF), (uint8_t)(total & 0xFF)
        };
        client_hello_raw.insert(client_hello_raw.end(), hdr, hdr + 6);
        client_hello_raw.insert(client_hello_raw.end(), client_hello.begin(), client_hello.end());
    }
    WHBLogPrintf("ap: handshake: sending ClientHello (%zu bytes)...",
                 client_hello_raw.size());
    if (!send_raw(client_hello_raw.data(), client_hello_raw.size())) {
        WHBLogPrint("ap: handshake: send ClientHello failed");
        mbedtls_mpi_free(&P_mpi); mbedtls_mpi_free(&X_mpi);
        return false;
    }
    WHBLogPrint("ap: handshake: ClientHello sent, waiting for APResponse...");

    // ── Receive APResponseMessage ─────────────────────────────────────────────
    std::vector<uint8_t> ap_msg, ap_raw;
    if (!recv_plain(ap_msg, ap_raw)) {
        mbedtls_mpi_free(&P_mpi); mbedtls_mpi_free(&X_mpi);
        WHBLogPrint("ap: recv APResponseMessage failed");
        return false;
    }

    // Parse APResponseMessage:
    //   field 10 = APChallenge (what we want)
    //   field 20 = UpgradeRequiredMessage (server rejects old client)
    //   field 30 = APLoginFailed (auth error)
    std::vector<uint8_t> server_pub;
    std::vector<uint8_t> server_nonce;

    PRd root = { ap_msg.data(), ap_msg.data() + ap_msg.size() };
    uint32_t f; uint8_t w;
    while (root.next(f, w)) {
        if (f == 10 && w == 2) {  // APChallenge
            PRd ap_hello; root.enter(ap_hello);
            while (ap_hello.next(f, w)) {
                if (f == 10 && w == 2) {  // login_crypto_challenge
                    PRd lc; ap_hello.enter(lc);
                    while (lc.next(f, w)) {
                        if (f == 10 && w == 2) {  // diffie_hellman
                            PRd dh; lc.enter(dh);
                            while (dh.next(f, w)) {
                                if (f == 10 && w == 2) dh.read_bytes(server_pub); // gs
                                else dh.skip(w);
                            }
                        } else lc.skip(w);
                    }
                } else if (f == 50 && w == 2) {  // server_nonce
                    ap_hello.read_bytes(server_nonce);
                } else ap_hello.skip(w);
            }
        } else if (f == 20 && w == 2) {  // UpgradeRequired
            std::vector<uint8_t> tmp; root.read_bytes(tmp);
            WHBLogPrint("ap: APResponse: UpgradeRequired — old client rejected");
        } else if (f == 30 && w == 2) {  // APLoginFailed
            PRd lf; root.enter(lf);
            uint64_t err = 0;
            while (lf.next(f, w)) {
                if (f == 10 && w == 0) lf.vi(err);
                else lf.skip(w);
            }
            WHBLogPrintf("ap: APResponse: APLoginFailed error_code=%llu", (unsigned long long)err);
        } else {
            WHBLogPrintf("ap: APResponse: unknown field %u wire %u", f, w);
            root.skip(w);
        }
    }

    if (server_pub.empty() || server_nonce.size() != 16) {
        mbedtls_mpi_free(&P_mpi); mbedtls_mpi_free(&X_mpi);
        WHBLogPrint("ap: bad APHello — missing server pubkey or nonce");
        return false;
    }
    // ── DH shared secret ──────────────────────────────────────────────────────
    mbedtls_mpi GY_mpi, S_mpi;
    mbedtls_mpi_init(&GY_mpi); mbedtls_mpi_init(&S_mpi);
    mbedtls_mpi_read_binary(&GY_mpi, server_pub.data(), server_pub.size());
    mbedtls_mpi_exp_mod(&S_mpi, &GY_mpi, &X_mpi, &P_mpi, nullptr);
    mbedtls_mpi_free(&GY_mpi);
    mbedtls_mpi_free(&X_mpi);
    mbedtls_mpi_free(&P_mpi);

    // Always 96 bytes — mbedtls_mpi_write_binary zero-pads if the value is smaller.
    // Using mpi_size() would give 95 bytes if the MSB is 0, causing HMAC mismatch.
    uint8_t shared[96] = {};
    mbedtls_mpi_write_binary(&S_mpi, shared, sizeof(shared));
    size_t shared_len = sizeof(shared);
    mbedtls_mpi_free(&S_mpi);

    // ── Key derivation: 5 × HMAC-SHA1(shared_secret, ch + ah + [i]) ──────────
    // [i] comes LAST — matches librespot (mac.update(packets); mac.update(&[i])).
    uint8_t key_data[100];
    for (int i = 1; i <= 5; ++i) {
        uint8_t ib = (uint8_t)i;
        hmac_sha1(shared, shared_len, {
            {client_hello_raw.data(), client_hello_raw.size()},
            {ap_raw.data(),           ap_raw.size()},
            {&ib, 1}
        }, key_data + (i-1)*20);
    }

    // From librespot compute_keys():
    //   challenge  = HMAC-SHA1(key=data[0..20],  msg=ch||ah)
    //   send_key   = data[20..52]  (32 bytes)
    //   recv_key   = data[52..84]  (32 bytes)
    send_cipher_ = std::make_unique<Shannon>(key_data + 20, 32);
    recv_cipher_ = std::make_unique<Shannon>(key_data + 52, 32);

    // ── Challenge response: HMAC-SHA1(key=data[0..20], ch + ah) ──────────────
    uint8_t challenge[20];
    hmac_sha1(key_data, 20, {
        {client_hello_raw.data(), client_hello_raw.size()},
        {ap_raw.data(),           ap_raw.size()}
    }, challenge);

    // ── Build and send ClientHelloResponse (= ClientResponsePlaintext) ───────
    // LoginCryptoDiffieHellmanResponse { hmac = challenge }
    std::vector<uint8_t> dh_resp;
    pb_bytes(dh_resp, 10, challenge, 20);

    // LoginCryptoResponseUnion { diffie_hellman = dh_resp }
    std::vector<uint8_t> crypto_resp;
    pb_msg(crypto_resp, 10, dh_resp);

    // ClientResponsePlaintext { login_crypto_response, pow_response, crypto_response }
    // pow_response and crypto_response are required in the proto — without them the
    // server's parser fails and returns BadCredentials (12) even if the HMAC is correct.
    std::vector<uint8_t> hello_resp;
    pb_msg(hello_resp, 10, crypto_resp);      // login_crypto_response
    std::vector<uint8_t> empty;
    pb_msg(hello_resp, 20, empty);             // pow_response (empty PoWResponseUnion)
    pb_msg(hello_resp, 30, empty);             // crypto_response (empty CryptoResponseUnion)

    if (!send_plain(hello_resp)) {
        WHBLogPrint("ap: send ClientHelloResponse failed");
        return false;
    }

    // Switch to raw POSIX recv for all post-handshake reads.
    // curl's CONNECT_ONLY mode appears to swallow incoming data after the
    // handshake, causing POLLIN to never fire on the raw fd.
    // Keep the socket O_NONBLOCK; the raw_recv_ path sleeps between EAGAIN
    // retries and has its own 30-second timeout.
    WHBLogPrint("ap: switched to raw recv");
    raw_recv_ = true;

    WHBLogPrint("ap: handshake complete");
    return true;
}

// ── do_login ──────────────────────────────────────────────────────────────────

bool AP::do_login(const Discovery::Credentials &creds) {
    WHBLogPrintf("ap: login: user='%s' auth_type=%u auth_data_len=%zu",
                 creds.username.c_str(), (unsigned)creds.auth_type,
                 creds.auth_data.size());

    // LoginCredentials { username, typ, auth_data }
    std::vector<uint8_t> login_creds;
    // LoginCredentials — field numbers from authentication.proto (hex in proto):
    //   username=0xa=10  typ=0x14=20  auth_data=0x1e=30
    pb_str(login_creds, 10, creds.username);
    pb_u32(login_creds, 20, creds.auth_type);
    pb_bytes(login_creds, 30, creds.auth_data.data(), creds.auth_data.size());

    // SystemInfo — field numbers from authentication.proto (hex in proto):
    //   cpu_family=0xa=10  brand=0x28=40  os=0x3c=60
    //   system_information_string=0x5a=90  device_id=0x64=100
    std::vector<uint8_t> sys_info;
    pb_u32(sys_info, 10, 0);  // CpuFamily::CPU_FAMILY_UNKNOWN
    pb_u32(sys_info, 40, 0);  // Brand::BRAND_UNBRANDED
    pb_u32(sys_info, 60, 0);  // Os::OS_UNKNOWN
    pb_str(sys_info, 90, "WiiU;1.1.0;en");
    if (!creds.device_id.empty())
        pb_str(sys_info, 100, creds.device_id);  // device_id

    // ClientResponseEncrypted { login_credentials(10), system_info(50) }
    std::vector<uint8_t> cre;
    pb_msg(cre, 10, login_creds);
    pb_msg(cre, 50, sys_info);

    WHBLogPrintf("ap: login: sending Login cmd=0xAB payload_len=%zu", cre.size());
    if (!send_packet(Cmd::Login, cre)) {
        WHBLogPrint("ap: login: send Login packet failed");
        return false;
    }
    WHBLogPrint("ap: login: Login packet sent, waiting for response...");

    // Do NOT sleep here — on the Wii U, poll() only fires POLLIN for data that
    // arrives *while* it is waiting.  A pre-recv sleep lets the response land in
    // the OS socket buffer before poll() is called, so POLLIN never fires and we
    // time out.  The handshake recv (no sleep) works fine for the same reason.

    // Receive APWelcome (0xAC) or AuthFailure (0xAD)
    // Read the first encrypted packet directly (not through the loop yet)
    uint8_t hdr[3];
    if (!recv_exact(hdr, 3)) {
        WHBLogPrint("ap: recv login response header failed");
        return false;
    }
    recv_cipher_->nonce(recv_seq_++);
    recv_cipher_->decrypt(hdr, 3);

    uint8_t cmd    = hdr[0];
    size_t  plen   = ((size_t)hdr[1] << 8) | hdr[2];
    WHBLogPrintf("ap: login response: cmd=0x%02X plen=%zu", cmd, plen);

    std::vector<uint8_t> payload(plen);
    if (!recv_exact(payload.data(), plen)) return false;
    recv_cipher_->decrypt(payload.data(), plen);

    uint8_t mac_buf[4], expected_mac[4];
    if (!recv_exact(mac_buf, 4)) return false;
    recv_cipher_->finish(expected_mac);
    if (memcmp(mac_buf, expected_mac, 4) != 0) {
        WHBLogPrint("ap: login response MAC mismatch");
        return false;
    }

    if (cmd == Cmd::APWelcome) {
        // APWelcome field numbers from authentication.proto (hex prefix = decimal value):
        //   canonical_username=0xa=10  reusable_auth_credentials_type=0x1e=30
        //   reusable_auth_credentials=0x28=40
        uint32_t reusable_type = 1;  // default: STORED_SPOTIFY_CREDENTIALS
        std::vector<uint8_t> reusable_data;
        PRd rd = { payload.data(), payload.data() + payload.size() };
        uint32_t f; uint8_t w;
        while (rd.next(f, w)) {
            if      (f == 10 && w == 2) rd.read_str(username_);
            else if (f == 30 && w == 0) { uint64_t v; rd.vi(v); reusable_type = (uint32_t)v; }
            else if (f == 40 && w == 2) rd.read_bytes(reusable_data);
            else rd.skip(w);
        }
        WHBLogPrintf("ap: logged in as '%s'", username_.c_str());

        // Load existing credentials from SD card into memory (for login5 in this session),
        // even if we'll also receive fresh credentials from APWelcome below.
        if (reusable_creds_.empty()) {
            FILE *fc = fopen("/vol/external01/spotify_saved_creds.bin", "rb");
            if (fc) {
                uint8_t tb; fread(&tb, 1, 1, fc);
                uint8_t buf[64]; size_t n = fread(buf, 1, sizeof(buf), fc);
                fclose(fc);
                if (n > 0) reusable_creds_.assign(buf, buf + n);
            }
        }

        if (!reusable_data.empty()) {
            WHBLogPrintf("ap: reusable creds type=%u len=%zu", reusable_type, reusable_data.size());
            // Store in memory for use by login5 token acquisition in this session.
            reusable_creds_ = reusable_data;
            // Always write to SD card — APWelcome credentials are long-lived stored
            // credentials regardless of how we authenticated (QR token, Zeroconf,
            // or prior saved creds).  Overwriting keeps creds fresh and lets a
            // QR login act as a one-shot setup for future launches.
            FILE *f2 = fopen("/vol/external01/spotify_saved_creds.bin", "wb");
            if (f2) {
                uint8_t tb = (uint8_t)reusable_type;
                fwrite(&tb, 1, 1, f2);
                fwrite(reusable_data.data(), 1, reusable_data.size(), f2);
                fclose(f2);
                WHBLogPrint("ap: saved creds written to SD card");
            }
        }
        return true;
    }

    if (cmd == Cmd::AuthFailure) {
        // Parse error code (field 10)
        uint32_t err_code = 0;
        PRd rd = { payload.data(), payload.data() + payload.size() };
        uint32_t f; uint8_t w;
        while (rd.next(f, w)) {
            if (f == 10 && w == 0) { uint64_t v; rd.vi(v); err_code = (uint32_t)v; }
            else rd.skip(w);
        }
        WHBLogPrintf("ap: auth failure, error_code=%u", err_code);
        return false;
    }

    WHBLogPrintf("ap: unexpected login response cmd=0x%02X", cmd);
    return false;
}

// ── connect ───────────────────────────────────────────────────────────────────

bool AP::connect(const Discovery::Credentials &creds, Callbacks cb) {
    callbacks_ = std::move(cb);

    std::vector<std::pair<std::string,uint16_t>> aps;
    resolve_ap(aps);

    for (auto &[h, p] : aps) {
        if (stop_.load()) break;
        if (!tcp_connect(h, p)) continue;
        if (stop_.load()) {
            if (curl_) { curl_easy_cleanup(curl_); curl_ = nullptr; } fd_ = -1; raw_recv_ = false; break;
        }
        if (!do_handshake()) {
            WHBLogPrintf("ap: handshake failed on %s:%u, trying next", h.c_str(), p);
            if (curl_) { curl_easy_cleanup(curl_); curl_ = nullptr; } fd_ = -1; raw_recv_ = false; continue;
        }
        if (stop_.load()) {
            if (curl_) { curl_easy_cleanup(curl_); curl_ = nullptr; } fd_ = -1; raw_recv_ = false; break;
        }
        if (!do_login(creds)) {
            WHBLogPrintf("ap: login failed on %s:%u, trying next", h.c_str(), p);
            if (curl_) { curl_easy_cleanup(curl_); curl_ = nullptr; } fd_ = -1; raw_recv_ = false; continue;
        }
        connected_.store(true);
        stop_.store(false);
        recv_thread_ = std::thread(&AP::recv_loop, this);
        return true;
    }
    if (!stop_.load())
        WHBLogPrint("ap: all AP candidates exhausted");
    return false;
}

// ── disconnect ────────────────────────────────────────────────────────────────

void AP::disconnect() {
    stop_.store(true);
    connected_.store(false);
    // shutdown() interrupts any blocked curl_easy_recv or raw recv.
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    if (recv_thread_.joinable()) recv_thread_.join();
    if (curl_) {
        curl_easy_cleanup(curl_);  // closes the socket
        curl_ = nullptr;
    } else if (fd_ >= 0) {
        close(fd_);
    }
    fd_ = -1;
}

// ── send_packet ───────────────────────────────────────────────────────────────

bool AP::send_packet(uint8_t cmd, const uint8_t *payload, size_t len) {
    if (!send_cipher_) {
        WHBLogPrintf("ap: send_packet 0x%02X: no cipher!", cmd);
        return false;
    }
    std::lock_guard<std::mutex> lock(send_mu_);

    send_cipher_->nonce(send_seq_++);

    // Build the entire wire packet in one buffer (hdr + payload + MAC) so it
    // goes out in a single send() call.  Sending the 4-byte MAC separately can
    // trigger Nagle buffering or curl internal hold-backs that stall delivery.
    std::vector<uint8_t> wire;
    wire.reserve(3 + len + 4);

    uint8_t hdr[3] = { cmd, (uint8_t)(len >> 8), (uint8_t)(len & 0xFF) };
    send_cipher_->encrypt(hdr, 3);
    wire.insert(wire.end(), hdr, hdr + 3);

    wire.insert(wire.end(), payload, payload + len);
    send_cipher_->encrypt(wire.data() + 3, len);

    uint8_t mac[4];
    send_cipher_->finish(mac);
    wire.insert(wire.end(), mac, mac + 4);

    if (!send_raw(wire.data(), wire.size())) {
        WHBLogPrintf("ap: send_packet 0x%02X: send failed", cmd);
        return false;
    }
    return true;
}

bool AP::send_packet(uint8_t cmd, const std::vector<uint8_t> &payload) {
    return send_packet(cmd, payload.data(), payload.size());
}

// ── recv_loop ─────────────────────────────────────────────────────────────────

void AP::recv_loop() {
    OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU0);
    while (!stop_.load()) {
        uint8_t hdr[3];
        if (!recv_exact(hdr, 3)) break;

        recv_cipher_->nonce(recv_seq_++);
        recv_cipher_->decrypt(hdr, 3);

        uint8_t cmd  = hdr[0];
        size_t  plen = ((size_t)hdr[1] << 8) | hdr[2];

        std::vector<uint8_t> payload(plen);
        if (!recv_exact(payload.data(), plen)) break;
        recv_cipher_->decrypt(payload.data(), plen);

        uint8_t mac_buf[4], expected[4];
        if (!recv_exact(mac_buf, 4)) break;
        recv_cipher_->finish(expected);

        if (memcmp(mac_buf, expected, 4) != 0) {
            WHBLogPrint("ap: MAC verification failed — closing");
            break;
        }

        // Handle internally
        if (cmd == Cmd::Ping) {
            if (!send_packet(Cmd::Pong, payload))
                WHBLogPrint("ap: Pong send failed");
            continue;
        }
        if (cmd == Cmd::CountryCode && payload.size() == 2) {
            country_code_ = std::string(reinterpret_cast<char *>(payload.data()), 2);
            WHBLogPrintf("ap: country code = %s", country_code_.c_str());
            continue;
        }
        if (cmd == Cmd::SecretBlock || cmd == Cmd::LegacyWelcome
            || cmd == Cmd::LicenseVersion || cmd == Cmd::ProductInfo) {
            continue; // discard
        }

        {
            char hex[64] = {};
            for (size_t i = 0; i < std::min(payload.size(), (size_t)16); ++i)
                snprintf(hex + i*3, 4, "%02X ", payload[i]);
            WHBLogPrintf("ap: recv cmd=0x%02X len=%zu: %s", cmd, payload.size(), hex);
        }
        if (callbacks_.on_packet)
            callbacks_.on_packet(cmd, std::move(payload));
    }

    connected_.store(false);
    if (callbacks_.on_disconnect) callbacks_.on_disconnect();
}

} // namespace Connect
