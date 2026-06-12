#include "zeroconf.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <curl/curl.h>

#include <mbedtls/dhm.h>
#include <mbedtls/sha1.h>
#include <mbedtls/md.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

#include <whb/log.h>
#include <whb/sdcard.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>

namespace Discovery {

static bool s_sd_mounted = false;

static void ensure_sd_mounted() {
    if (!s_sd_mounted) {
        s_sd_mounted = WHBMountSdCard();
        WHBLogPrintf("zc: SD card %s", s_sd_mounted ? "mounted" : "FAILED to mount");
    }
}


// ── DH parameters (RFC 2409 / MODP Group 2, 1024-bit) ────────────────────────
// Generator g = 2; prime p is standard.
// RFC 2409 Oakley Group 2 — 1024-bit MODP prime, generator 2.
// Spotify Connect uses this exact group for the Zeroconf DH key exchange.
static const uint8_t DH_PRIME[128] = {
    // FFFFFFFF FFFFFFFF C90FDAA2 21468C23 4C4C6628 B80DC1CD
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC9, 0x0F, 0xDA, 0xA2,
    0x21, 0x46, 0x8C, 0x23, 0x4C, 0x4C, 0x66, 0x28, 0xB8, 0x0D, 0xC1, 0xCD,
    // 29024E08 8A67CC74 020BBEA6 3B139B22 514A0879 8E3404DD
    0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74, 0x02, 0x0B, 0xBE, 0xA6,
    0x3B, 0x13, 0x9B, 0x22, 0x51, 0x4A, 0x08, 0x79, 0x8E, 0x34, 0x04, 0xDD,
    // EF9519B3 CD3A431B 302B0A6D F25F1437 4FE1356D 6D51C245
    0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B, 0x30, 0x2B, 0x0A, 0x6D,
    0xF2, 0x5F, 0x14, 0x37, 0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45,
    // E485B576 625E7EC6 F44C42E9 A637ED6B 0BFF5CB6 F406B7ED
    0xE4, 0x85, 0xB5, 0x76, 0x62, 0x5E, 0x7E, 0xC6, 0xF4, 0x4C, 0x42, 0xE9,
    0xA6, 0x37, 0xED, 0x6B, 0x0B, 0xFF, 0x5C, 0xB6, 0xF4, 0x06, 0xB7, 0xED,
    // EE386BFB 5A899FA5 AE9F2411 7C4B1FE6 49286651 ECE65381
    0xEE, 0x38, 0x6B, 0xFB, 0x5A, 0x89, 0x9F, 0xA5, 0xAE, 0x9F, 0x24, 0x11,
    0x7C, 0x4B, 0x1F, 0xE6, 0x49, 0x28, 0x66, 0x51, 0xEC, 0xE6, 0x53, 0x81,
    // FFFFFFFF FFFFFFFF
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
static const uint8_t DH_GENERATOR[1] = { 0x02 };

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string b64_encode(const uint8_t *data, size_t len) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    std::string out(olen, '\0');
    mbedtls_base64_encode(reinterpret_cast<uint8_t *>(&out[0]), olen, &olen,
                          data, len);
    out.resize(olen);
    return out;
}

static std::vector<uint8_t> b64_decode(const std::string &s) {
    size_t olen = 0;
    const auto *src = reinterpret_cast<const uint8_t *>(s.data());
    mbedtls_base64_decode(nullptr, 0, &olen, src, s.size());
    std::vector<uint8_t> out(olen);
    mbedtls_base64_decode(out.data(), olen, &olen, src, s.size());
    out.resize(olen);
    return out;
}

static char hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

static std::string url_decode(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)((hex_nibble(s[i+1]) << 4) | hex_nibble(s[i+2]));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::map<std::string, std::string> parse_params(const std::string &body) {
    std::map<std::string, std::string> m;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eq  = body.find('=', pos);
        size_t amp = body.find('&', pos);
        if (eq == std::string::npos) break;
        size_t end = (amp == std::string::npos) ? body.size() : amp;
        std::string key = url_decode(body.substr(pos, eq - pos));
        std::string val = url_decode(body.substr(eq + 1, end - eq - 1));
        m[key] = val;
        pos = (amp == std::string::npos) ? body.size() : amp + 1;
    }
    return m;
}

// Get the query-string value of 'action' from a path like /zc?action=getInfo
static std::string get_action(const std::string &path) {
    auto q = path.find("action=");
    if (q == std::string::npos) return {};
    size_t end = path.find('&', q);
    std::string v = path.substr(q + 7,
                                end == std::string::npos ? std::string::npos
                                                         : end - q - 7);
    return v;
}

// ── mDNS packet helpers ───────────────────────────────────────────────────────

static void put_u8(std::vector<uint8_t> &b, uint8_t v) {
    b.push_back(v);
}
static void put_u16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(v >> 8); b.push_back(v & 0xFF);
}
static void put_u32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(v >> 24); b.push_back((v >> 16) & 0xFF);
    b.push_back((v >>  8) & 0xFF); b.push_back(v & 0xFF);
}

// Append a DNS name (dot-separated labels, may end with '.').
static void put_dns_name(std::vector<uint8_t> &b, const std::string &name) {
    size_t pos = 0;
    while (pos < name.size()) {
        size_t dot = name.find('.', pos);
        size_t end = (dot == std::string::npos) ? name.size() : dot;
        size_t len = end - pos;
        if (len == 0) { pos++; continue; }
        put_u8(b, (uint8_t)len);
        for (size_t i = pos; i < end; i++) put_u8(b, (uint8_t)name[i]);
        pos = end + 1;
    }
    put_u8(b, 0x00); // root terminator
}

// Append a complete RR (name + type + class + TTL + rdlength + rdata).
static void put_rr(std::vector<uint8_t> &b,
                   const std::string &name, uint16_t type, uint16_t cls,
                   uint32_t ttl, const std::vector<uint8_t> &rdata) {
    put_dns_name(b, name);
    put_u16(b, type);
    put_u16(b, cls);
    put_u32(b, ttl);
    put_u16(b, (uint16_t)rdata.size());
    b.insert(b.end(), rdata.begin(), rdata.end());
}

// ── Local IP detection ────────────────────────────────────────────────────────

static uint32_t get_local_ip() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(53);
    dst.sin_addr.s_addr = inet_addr("224.0.0.251");
    connect(fd, reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst));
    struct sockaddr_in local{};
    socklen_t slen = sizeof(local);
    getsockname(fd, reinterpret_cast<struct sockaddr *>(&local), &slen);
    close(fd);
    return local.sin_addr.s_addr; // network byte order
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

Zeroconf::Zeroconf(const std::string &device_name,
                   const std::string &device_id)
    : device_name_(device_name), device_id_(device_id)
{
    // DH key pair — heap-allocate RNG state (stack entropy can hang on Wii U)
    auto *entropy  = new mbedtls_entropy_context;
    auto *ctr_drbg = new mbedtls_ctr_drbg_context;
    mbedtls_entropy_init(entropy);
    mbedtls_ctr_drbg_init(ctr_drbg);
    mbedtls_ctr_drbg_seed(ctr_drbg, mbedtls_entropy_func, entropy,
                          reinterpret_cast<const uint8_t *>("zc"), 2);

    mbedtls_dhm_init(&dhm_);
    mbedtls_mpi_read_binary(&dhm_.P, DH_PRIME, sizeof(DH_PRIME));
    mbedtls_mpi_read_binary(&dhm_.G, DH_GENERATOR, sizeof(DH_GENERATOR));

    // Generate our public key (X = g^x mod p)
    uint8_t pub[128];
    mbedtls_dhm_make_public(&dhm_, (int)mbedtls_mpi_size(&dhm_.P),
                            pub, sizeof(pub),
                            mbedtls_ctr_drbg_random, ctr_drbg);
    public_key_b64_ = b64_encode(pub, sizeof(pub));

    mbedtls_ctr_drbg_free(ctr_drbg);
    mbedtls_entropy_free(entropy);
    delete ctr_drbg;
    delete entropy;
}

Zeroconf::~Zeroconf() {
    stop();
    mbedtls_dhm_free(&dhm_);
}

void Zeroconf::start(std::function<void(Credentials)> on_creds,
                     std::function<void(std::string)> on_error) {
    on_creds_ = std::move(on_creds);
    on_error_ = std::move(on_error);
    stop_.store(false);
    mdns_thread_ = std::thread(&Zeroconf::mdns_thread_fn, this);
    http_thread_ = std::thread(&Zeroconf::http_thread_fn, this);
}

void Zeroconf::stop() {
    stop_.store(true);
    if (mdns_thread_.joinable()) mdns_thread_.join();
    if (http_thread_.joinable()) http_thread_.join();
}

// ── mDNS announcer ────────────────────────────────────────────────────────────

void Zeroconf::mdns_thread_fn() {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { WHBLogPrint("zc: mdns socket failed"); return; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(5353);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    int bret = bind(sock, reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr));
    WHBLogPrintf("zc: mdns bind=%d errno=%d", bret, errno);

    uint8_t ttl = 255;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    uint32_t local_ip = get_local_ip();
    char ip_str[32];
    struct in_addr ia; ia.s_addr = local_ip;
    inet_ntop(AF_INET, &ia, ip_str, sizeof(ip_str));
    WHBLogPrintf("zc: mdns local_ip=%s", ip_str);

    // Join the mDNS multicast group so IGMP snooping routers forward the packets
    struct ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    mreq.imr_interface.s_addr = local_ip;
    int jret = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    WHBLogPrintf("zc: mdns join=%d errno=%d", jret, errno);

    // Pin outgoing multicast to the correct interface
    struct in_addr iface{}; iface.s_addr = local_ip;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));

    // Make socket non-blocking so we can both recv queries and send announcements.
    fcntl(sock, F_SETFL, O_NONBLOCK);

    int countdown = 0;
    while (!stop_.load()) {
        // Respond to incoming PTR queries for _spotify-connect._tcp.local
        uint8_t buf[512];
        struct sockaddr_in src{};
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             reinterpret_cast<struct sockaddr *>(&src), &slen);
        if (n > 12) {
            // DNS header: flags at offset 2 (QR=0 means query)
            uint16_t flags = (uint16_t)((buf[2] << 8) | buf[3]);
            uint16_t qdcount = (uint16_t)((buf[4] << 8) | buf[5]);
            bool is_query = !(flags & 0x8000);
            if (is_query && qdcount > 0) {
                // Walk questions; check for PTR query matching our service type
                size_t pos = 12;
                for (uint16_t q = 0; q < qdcount && pos < (size_t)n; q++) {
                    // Decode name
                    std::string qname;
                    while (pos < (size_t)n && buf[pos] != 0) {
                        uint8_t llen = buf[pos++];
                        if (llen >= 0xC0) { pos++; break; } // pointer — skip
                        for (uint8_t i = 0; i < llen && pos < (size_t)n; i++)
                            qname += (char)buf[pos++];
                        qname += '.';
                    }
                    if (buf[pos] == 0) pos++; // root terminator
                    uint16_t qtype  = pos + 1 < (size_t)n ? (uint16_t)((buf[pos] << 8) | buf[pos+1]) : 0;
                    pos += 4; // skip qtype + qclass

                    // PTR query (type 12) for our service?
                    bool match = qtype == 12 &&
                                 (qname.find("_spotify-connect") != std::string::npos);
                    if (match) {
                        WHBLogPrint("zc: mdns PTR query — replying");
                        mdns_announce(sock, local_ip);
                        break;
                    }
                }
            }
        }

        if (countdown == 0) {
            WHBLogPrint("zc: mdns announcing...");
            mdns_announce(sock, local_ip);
            countdown = 30;
        }
        OSSleepTicks(OSMillisecondsToTicks(1000));
        --countdown;
    }
    close(sock);
}

void Zeroconf::mdns_announce(int sock, uint32_t local_ip) {
    const std::string svc_type  = "_spotify-connect._tcp.local";
    const std::string inst_name = device_name_ + "." + svc_type;
    const std::string host_name = device_name_ + ".local";

    std::vector<uint8_t> pkt;
    pkt.reserve(512);

    // DNS header
    put_u16(pkt, 0x0000); // ID = 0
    put_u16(pkt, 0x8400); // QR=1, AA=1
    put_u16(pkt, 0x0000); // QDCOUNT
    put_u16(pkt, 0x0004); // ANCOUNT = 4 records
    put_u16(pkt, 0x0000); // NSCOUNT
    put_u16(pkt, 0x0000); // ARCOUNT

    // PTR: _spotify-connect._tcp.local → instance name
    {
        std::vector<uint8_t> rd;
        put_dns_name(rd, inst_name);
        put_rr(pkt, svc_type, 0x000C, 0x0001, 4500, rd);
    }

    // SRV: instance → hostname:port
    {
        std::vector<uint8_t> rd;
        put_u16(rd, 0);          // priority
        put_u16(rd, 0);          // weight
        put_u16(rd, (uint16_t)http_port_);
        put_dns_name(rd, host_name);
        put_rr(pkt, inst_name, 0x0021, 0x8001, 120, rd);
    }

    // TXT: key=value pairs for the service
    {
        std::vector<uint8_t> rd;
        const char *txts[] = { "VERSION=1.0", "CPath=/zc", "Stack=SP" };
        for (const char *t : txts) {
            uint8_t len = (uint8_t)strlen(t);
            rd.push_back(len);
            rd.insert(rd.end(), t, t + len);
        }
        put_rr(pkt, inst_name, 0x0010, 0x8001, 4500, rd);
    }

    // A: hostname → local IPv4
    {
        std::vector<uint8_t> rd(4);
        memcpy(rd.data(), &local_ip, 4);
        put_rr(pkt, host_name, 0x0001, 0x8001, 120, rd);
    }

    struct sockaddr_in mcast{};
    mcast.sin_family      = AF_INET;
    mcast.sin_port        = htons(5353);
    mcast.sin_addr.s_addr = inet_addr("224.0.0.251");
    ssize_t sent = sendto(sock, pkt.data(), pkt.size(), 0,
                          reinterpret_cast<struct sockaddr *>(&mcast), sizeof(mcast));
    WHBLogPrintf("zc: mdns sendto=%zd pkt=%zu errno=%d", sent, pkt.size(), errno);
}

// ── HTTP server ───────────────────────────────────────────────────────────────

void Zeroconf::http_thread_fn() {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { WHBLogPrint("zc: http socket failed"); return; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    fcntl(srv, F_SETFL, O_NONBLOCK);

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(http_port_);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        WHBLogPrint("zc: http bind failed"); close(srv); return;
    }
    listen(srv, 4);
    WHBLogPrintf("zc: HTTP listening on port %d", http_port_);

    while (!stop_.load()) {
        struct pollfd pfd = { srv, POLLIN, 0 };
        if (poll(&pfd, 1, 200) <= 0) continue;
        int fd = accept(srv, nullptr, nullptr);
        if (fd >= 0) {
            fcntl(fd, F_SETFL, 0); // clear O_NONBLOCK inherited from listening socket
            handle_client(fd);
            close(fd);
        }
    }
    close(srv);
}

void Zeroconf::handle_client(int fd) {
    std::string req = http_read(fd);
    if (req.empty()) return;

    // Parse first line: "METHOD /path HTTP/1.x"
    size_t sp1 = req.find(' ');
    size_t sp2 = req.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    std::string method = req.substr(0, sp1);
    std::string path   = req.substr(sp1 + 1, sp2 - sp1 - 1);

    // Extract HTTP body (everything after the blank line)
    std::string body_raw;
    size_t body_start = std::string::npos;
    auto crnl = req.find("\r\n\r\n");
    if (crnl != std::string::npos) body_start = crnl + 4;
    else { auto nl = req.find("\n\n"); if (nl != std::string::npos) body_start = nl + 2; }
    if (body_start != std::string::npos) {
        body_raw = req.substr(body_start);
        while (!body_raw.empty() &&
               (body_raw.back() == '\r' || body_raw.back() == '\n' || body_raw.back() == ' '))
            body_raw.pop_back();
    }

    // action= may be in the query string (GET) or in the URL-encoded body (POST)
    std::string action = get_action(path);
    std::map<std::string, std::string> params;
    if (action.empty() && !body_raw.empty()) {
        params = parse_params(body_raw);
        action = params["action"];
    }

    WHBLogPrintf("zc: %s %s action=%s body_len=%zu",
                 method.c_str(), path.c_str(), action.c_str(), body_raw.size());
    if (!body_raw.empty())
        WHBLogPrintf("zc: body[0..200]: %.200s", body_raw.c_str());

    if (action == "getInfo") {
        http_respond(fd, 200, "application/json", build_info_json());
        return;
    }

    if (action == "addUser") {
        // Params may already be parsed above; if not, parse now
        if (params.empty()) params = parse_params(body_raw);

        // Log every param so we can see exactly what the Spotify app sends.
        for (auto &[k, v] : params)
            WHBLogPrintf("zc: addUser param '%s'='%.80s'(len=%zu)", k.c_str(), v.c_str(), v.size());

        const std::string &username      = params["userName"];
        const std::string &blob_b64      = params["blob"];
        const std::string &client_key    = params["clientKey"];
        const std::string &login_id      = params["loginId"];
        // Newer Spotify apps may send a full OAuth token in a separate field.
        const std::string &access_token  = params["accessToken"];
        const std::string &token_field   = params["token"];

        Credentials creds;
        creds.device_id = device_id_;  // always include our Wii U device ID
        bool ok = false;

        if (!username.empty() && !blob_b64.empty() && !client_key.empty()) {
            // Old protocol: DH-encrypted blob
            ok = decrypt_blob(username, b64_decode(client_key),
                              b64_decode(blob_b64), creds);
            if (!ok) WHBLogPrint("zc: blob decrypt failed");
        } else if (!username.empty() && (!access_token.empty() || !token_field.empty())) {
            // Prefer a dedicated token field over loginId (loginId is a UUID, not an OAuth token).
            const std::string &tok = access_token.empty() ? token_field : access_token;
            WHBLogPrintf("zc: token auth via accessToken/token len=%zu first20='%.20s'",
                         tok.size(), tok.c_str());
            creds.username  = username;
            creds.auth_type = 0x03;  // AUTHENTICATION_SPOTIFY_TOKEN
            creds.auth_data.assign(tok.begin(), tok.end());
            ok = true;
        } else if (!username.empty() && !login_id.empty()) {
            WHBLogPrintf("zc: loginId flow user='%s' loginId='%.32s'",
                         username.c_str(), login_id.c_str());

            // Respond immediately so the Spotify app doesn't time out.
            http_respond(fd, 200, "application/json",
                         "{\"status\":101,\"statusString\":\"OK\","
                         "\"spotifyError\":0}");

            ensure_sd_mounted();

            creds.username = username;

            // Priority 1: saved reusable credentials from a previous APWelcome.
            {
                FILE *f = fopen("/vol/external01/spotify_saved_creds.bin", "rb");
                if (f) {
                    uint8_t type_byte = 0;
                    fread(&type_byte, 1, 1, f);
                    std::vector<uint8_t> blob;
                    uint8_t buf[512];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                        blob.insert(blob.end(), buf, buf + n);
                    fclose(f);
                    if (!blob.empty()) {
                        creds.auth_type = type_byte;
                        creds.auth_data = std::move(blob);
                        WHBLogPrintf("zc: using saved creds auth_type=%u len=%zu",
                                     (unsigned)creds.auth_type, creds.auth_data.size());
                        if (on_creds_) on_creds_(std::move(creds));
                        return;
                    }
                }
            }

            WHBLogPrint("zc: spotify_saved_creds.bin not found on SD card");
            if (on_error_) on_error_("Credentials not found on SD card");
            return;  // already responded above
        } else {
            WHBLogPrintf("zc: addUser missing credentials user='%s' blob_len=%zu loginId_len=%zu",
                         username.c_str(), blob_b64.size(), login_id.size());
        }

        if (ok) {
            http_respond(fd, 200, "application/json",
                         "{\"status\":101,\"statusString\":\"OK\","
                         "\"spotifyError\":0}");
            if (on_creds_) on_creds_(std::move(creds));
        } else {
            http_respond(fd, 200, "application/json",
                         "{\"status\":403,\"statusString\":\"ERROR-INVALID-CREDENTIALS\","
                         "\"spotifyError\":0}");
        }
        return;
    }

    http_respond(fd, 404, "text/plain", "Not found");
}

// ── getInfo JSON ──────────────────────────────────────────────────────────────

std::string Zeroconf::build_info_json() const {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{"
        "\"status\":101,"
        "\"statusString\":\"OK\","
        "\"spotifyError\":0,"
        "\"version\":\"2.1.0\","
        "\"libraryVersion\":\"0.1.0\","
        "\"accountReq\":\"PREMIUM\","
        "\"brandDisplayName\":\"Nintendo\","
        "\"modelDisplayName\":\"Wii U\","
        "\"deviceType\":\"GAME_CONSOLE\","
        "\"remoteName\":\"%s\","
        "\"activeUser\":\"%s\","
        "\"deviceID\":\"%s\","
        "\"publicKey\":\"%s\","
        "\"scope\":\"streaming\""
        "}",
        device_name_.c_str(),
        active_user_.c_str(),
        device_id_.c_str(),
        public_key_b64_.c_str());
    return buf;
}

// ── Blob decryption ───────────────────────────────────────────────────────────
//
// Protocol:
//   1. DH shared secret = dhm_calc_secret(client_pub_key)
//   2. base_key = SHA1(shared_secret)[0:16]
//   3. enc_key  = HMAC-SHA1(key=base_key, msg="encryption")[0:16]
//   4. mac_key  = HMAC-SHA1(key=base_key, msg="checksum")
//   5. Verify   HMAC-SHA1(key=mac_key, msg=blob[0:-20]) == blob[-20:]
//   6. Decrypt  AES-CTR(key=enc_key, nonce=0, ciphertext=blob[0:-20])
//   7. Parse    decrypted blob → username + auth_type + auth_data

bool Zeroconf::decrypt_blob(const std::string &username,
                             const std::vector<uint8_t> &client_key,
                             const std::vector<uint8_t> &blob,
                             Credentials &out)
{
    if (blob.size() < 21) return false; // need at least 1 byte data + 20 MAC

    // 1. DH shared secret
    if (mbedtls_dhm_read_public(&dhm_, client_key.data(), client_key.size()) != 0)
        return false;

    uint8_t shared[128];
    size_t  shared_len = sizeof(shared);
    // RNG not needed for calc_secret when using standard groups
    if (mbedtls_dhm_calc_secret(&dhm_, shared, sizeof(shared), &shared_len,
                                 nullptr, nullptr) != 0)
        return false;

    // 2. base_key = SHA1(shared_secret)
    uint8_t sha_out[20];
    mbedtls_sha1(shared, shared_len, sha_out);
    // Use first 16 bytes as the HMAC key
    const uint8_t *base_key = sha_out;

    // 3. enc_key = HMAC-SHA1(base_key, "encryption")[0:16]
    uint8_t enc_full[20];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
                    base_key, 16,
                    reinterpret_cast<const uint8_t *>("encryption"),
                    strlen("encryption"), enc_full);

    // 4. mac_key = HMAC-SHA1(base_key, "checksum")
    uint8_t mac_key[20];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
                    base_key, 16,
                    reinterpret_cast<const uint8_t *>("checksum"),
                    strlen("checksum"), mac_key);

    // 5. Verify MAC over blob[0:-20]
    size_t data_len = blob.size() - 20;
    uint8_t mac_computed[20];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
                    mac_key, 20,
                    blob.data(), data_len, mac_computed);
    if (memcmp(mac_computed, blob.data() + data_len, 20) != 0) {
        WHBLogPrint("zc: blob MAC mismatch");
        return false;
    }

    // 6. Decrypt with AES-CTR, zero nonce
    std::vector<uint8_t> plain(data_len);
    {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, enc_full, 128);
        uint8_t nonce[16] = {};
        uint8_t stream_block[16] = {};
        size_t nc_off = 0;
        mbedtls_aes_crypt_ctr(&aes, data_len, &nc_off,
                              nonce, stream_block,
                              blob.data(), plain.data());
        mbedtls_aes_free(&aes);
    }

    // 7. Parse: [len_user][username][auth_type][len_data][auth_data]
    size_t pos = 0;
    auto read_len = [&]() -> int {
        if (pos >= plain.size()) return -1;
        uint8_t b0 = plain[pos++];
        if (b0 & 0x80) {
            if (pos >= plain.size()) return -1;
            return ((b0 & 0x7F) << 8) | plain[pos++];
        }
        return b0;
    };

    int ulen = read_len();
    if (ulen < 0 || pos + (size_t)ulen + 1 > plain.size()) return false;
    // username in blob must match the POST param
    std::string blob_user(reinterpret_cast<char *>(plain.data() + pos), ulen);
    if (blob_user != username) {
        WHBLogPrintf("zc: username mismatch blob='%s' param='%s'",
                     blob_user.c_str(), username.c_str());
        // Non-fatal — use POST-provided username
    }
    pos += ulen;

    if (pos >= plain.size()) return false;
    out.auth_type = plain[pos++];

    int alen = read_len();
    if (alen < 0 || pos + (size_t)alen > plain.size()) return false;
    out.auth_data.assign(plain.data() + pos, plain.data() + pos + alen);
    out.username = username;

    WHBLogPrintf("zc: credentials for '%s' auth_type=%d auth_data_len=%d",
                 out.username.c_str(), out.auth_type, alen);
    return true;
}

// ── HTTP helpers ──────────────────────────────────────────────────────────────

std::string Zeroconf::http_read(int fd) {
    std::string req;
    req.reserve(2048);
    char buf[512];
    while (req.size() < 8192) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, n);
        // Stop once we have the full headers (and for POST, the body)
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos) continue;

        // Check Content-Length
        auto cl_pos = req.find("Content-Length:");
        if (cl_pos == std::string::npos) cl_pos = req.find("content-length:");
        if (cl_pos == std::string::npos) break; // no body expected

        size_t cl_val = std::stoul(req.substr(cl_pos + 15));
        size_t body_start = hdr_end + 4;
        size_t body_have  = req.size() - body_start;
        if (body_have >= cl_val) break;
        // Read remaining body bytes
        while (body_have < cl_val) {
            n = recv(fd, buf, (int)std::min(sizeof(buf), cl_val - body_have), 0);
            if (n <= 0) break;
            req.append(buf, n);
            body_have += n;
        }
        break;
    }
    return req;
}

void Zeroconf::http_respond(int fd, int status, const std::string &ct,
                             const std::string &body) {
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 %d OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             status, ct.c_str(), body.size());
    send(fd, hdr, strlen(hdr), 0);
    if (!body.empty()) send(fd, body.data(), body.size(), 0);
}

} // namespace Discovery
