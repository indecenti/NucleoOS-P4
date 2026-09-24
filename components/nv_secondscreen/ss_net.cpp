// Socket / HTTP / WebSocket plumbing for the Second Screen network transports (see ss_net.h).
#include "ss_net.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/ssl.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "esp_timer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <strings.h>

// ---------------------------------------------------------------- connection
int SsConn::recv(void *buf, size_t n) {
    if (fd < 0) return -1;
    if (tls) {
        int r = mbedtls_ssl_read((mbedtls_ssl_context *)tls, (unsigned char *)buf, n);
        if (r > 0) return r;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_TIMEOUT) return -2;
        return -1;
    }
    int r = ::recv(fd, buf, n, 0);
    if (r > 0) return r;
    if (r == 0) return 0;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : -1;
}

bool SsConn::send_all(const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    int stalls = 0;
    while (n > 0) {
        int r;
        if (tls) {
            r = mbedtls_ssl_write((mbedtls_ssl_context *)tls, p, n);
            if (r == MBEDTLS_ERR_SSL_WANT_WRITE) r = -2;
        } else {
            r = ::send(fd, p, n, 0);
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) r = -2;
        }
        if (r == -2) { if (++stalls > 20) return false; continue; }   // ~20 send timeouts
        if (r <= 0) return false;
        stalls = 0;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

void SsConn::set_timeout_ms(int ms) {
    if (fd < 0) return;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct timeval tw = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tw, sizeof tw);
}

void SsConn::close() {
    if (tls) {
        auto *ssl = (mbedtls_ssl_context *)tls;
        mbedtls_ssl_close_notify(ssl);
        mbedtls_ssl_free(ssl);
        free(ssl);
        tls = nullptr;
    }
    if (fd >= 0) { shutdown(fd, SHUT_RDWR); ::close(fd); fd = -1; }
}

bool ss_read_exact(SsConn &c, void *buf, size_t n, int deadline_ms) {
    uint8_t *p = (uint8_t *)buf;
    int64_t last = esp_timer_get_time();
    while (n > 0) {
        int r = c.recv(p, n);
        if (r > 0) { p += r; n -= (size_t)r; last = esp_timer_get_time(); continue; }
        if (r == -2 && (esp_timer_get_time() - last) / 1000 < deadline_ms) continue;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- HTTP
static bool header_is(const char *line, const char *name, const char **val) {
    const size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':') return false;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *val = v;
    return true;
}

static void copy_trim(char *dst, size_t n, const char *src) {
    size_t i = 0;
    while (src[i] && src[i] != '\r' && src[i] != '\n' && i + 1 < n) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

bool ss_http_read_head(SsConn &c, SsHttpReq &r, int timeout_ms) {
    char buf[4096];
    size_t len = 0;
    const int64_t t0 = esp_timer_get_time();
    // Read byte-chunks until the blank line. Requests are tiny GETs; any body is ignored.
    while (len < sizeof buf - 1) {
        int n = c.recv(buf + len, sizeof buf - 1 - len);
        if (n > 0) {
            len += (size_t)n;
            buf[len] = 0;
            if (strstr(buf, "\r\n\r\n")) break;
            continue;
        }
        if (n == -2 && (esp_timer_get_time() - t0) / 1000 < timeout_ms) continue;
        return false;
    }
    buf[len] = 0;
    // request line
    char *sp1 = strchr(buf, ' ');
    if (!sp1) return false;
    *sp1 = 0;
    snprintf(r.method, sizeof r.method, "%s", buf);
    char *uri = sp1 + 1;
    char *sp2 = strchr(uri, ' ');
    if (!sp2) return false;
    *sp2 = 0;
    char *q = strchr(uri, '?');
    if (q) { *q = 0; snprintf(r.query, sizeof r.query, "%s", q + 1); }
    snprintf(r.path, sizeof r.path, "%s", uri);
    // headers
    char *line = strstr(sp2 + 1, "\r\n");
    bool upgrade = false, conn_upgrade = false;
    while (line) {
        line += 2;
        if (*line == '\r' || !*line) break;
        const char *v;
        if (header_is(line, "Host", &v)) copy_trim(r.host, sizeof r.host, v);
        else if (header_is(line, "User-Agent", &v)) copy_trim(r.ua, sizeof r.ua, v);
        else if (header_is(line, "Sec-WebSocket-Key", &v)) copy_trim(r.ws_key, sizeof r.ws_key, v);
        else if (header_is(line, "Upgrade", &v)) upgrade = strncasecmp(v, "websocket", 9) == 0;
        else if (header_is(line, "Connection", &v)) {
            char tmp[64]; copy_trim(tmp, sizeof tmp, v);
            for (char *p = tmp; *p; p++) *p = (char)tolower((unsigned char)*p);
            conn_upgrade = strstr(tmp, "upgrade") != nullptr;
        }
        line = strstr(line, "\r\n");
    }
    r.upgrade_ws = upgrade && conn_upgrade && r.ws_key[0];
    return true;
}

static const char *status_text(int code) {
    switch (code) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

bool ss_http_send_head(SsConn &c, int code, const char *ctype, size_t len, const char *extra) {
    char h[640];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                     code, status_text(code), ctype ? ctype : "text/plain", (unsigned)len,
                     extra ? extra : "");
    return n > 0 && n < (int)sizeof h && c.send_all(h, (size_t)n);
}

bool ss_http_send(SsConn &c, int code, const char *ctype, const void *body, size_t len, const char *extra) {
    if (!ss_http_send_head(c, code, ctype, len, extra)) return false;
    return len == 0 || c.send_all(body, len);
}

bool ss_query_get(const char *query, const char *key, char *out, size_t n) {
    if (!query || !key || !out || !n) return false;
    const size_t kl = strlen(key);
    const char *p = query;
    while (*p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < n) {
                if (*p == '%' && p[1] && p[2]) {
                    char hx[3] = {p[1], p[2], 0};
                    out[i++] = (char)strtol(hx, nullptr, 16);
                    p += 3;
                } else if (*p == '+') { out[i++] = ' '; p++; }
                else out[i++] = *p++;
            }
            out[i] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return false;
}

// ---------------------------------------------------------------- WebSocket
bool ss_ws_accept(SsConn &c, const SsHttpReq &r) {
    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char cat[80];
    snprintf(cat, sizeof cat, "%s%s", r.ws_key, kGuid);
    unsigned char sha[20];
    mbedtls_sha1((const unsigned char *)cat, strlen(cat), sha);
    unsigned char b64[40];
    size_t olen = 0;
    mbedtls_base64_encode(b64, sizeof b64, &olen, sha, sizeof sha);
    b64[olen] = 0;
    char h[256];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", (const char *)b64);
    return c.send_all(h, (size_t)n);
}

bool ss_ws_send(SsConn &c, SsWsOp op, const void *data, size_t len) {
    uint8_t h[10];
    size_t hl = 2;
    h[0] = 0x80 | (uint8_t)op;
    if (len < 126) h[1] = (uint8_t)len;
    else if (len < 65536) { h[1] = 126; h[2] = (uint8_t)(len >> 8); h[3] = (uint8_t)len; hl = 4; }
    else {
        h[1] = 127;
        for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        hl = 10;
    }
    // One write when small (fewer TCP segments for acks/touch events).
    if (len <= 256) {
        uint8_t buf[266];
        memcpy(buf, h, hl);
        if (len) memcpy(buf + hl, data, len);
        return c.send_all(buf, hl + len);
    }
    return c.send_all(h, hl) && c.send_all(data, len);
}

bool ss_ws_send_text(SsConn &c, const char *s) { return ss_ws_send(c, WS_TEXT, s, strlen(s)); }

int ss_ws_read_header(SsConn &c, SsWsFrame &f, int idle_ms) {
    uint8_t h[2];
    // First byte: distinguish "idle" (no frame started) from a broken link.
    int r = c.recv(h, 1);
    if (r == -2) return -2;
    if (r <= 0) return 0;
    if (!ss_read_exact(c, h + 1, 1, idle_ms)) return 0;
    f.fin = (h[0] & 0x80) != 0;
    f.op = h[0] & 0x0F;
    f.masked = (h[1] & 0x80) != 0;
    uint64_t len = h[1] & 0x7F;
    if (len == 126) {
        uint8_t e[2];
        if (!ss_read_exact(c, e, 2, idle_ms)) return 0;
        len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        uint8_t e[8];
        if (!ss_read_exact(c, e, 8, idle_ms)) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    f.len = len;
    if (f.masked && !ss_read_exact(c, f.mask, 4, idle_ms)) return 0;
    return 1;
}

bool ss_ws_read_payload(SsConn &c, const SsWsFrame &f, uint64_t offset, uint8_t *dst, size_t n) {
    uint8_t scratch[512];
    size_t done = 0;
    while (done < n) {
        uint8_t *p = dst ? dst + done : scratch;
        size_t want = n - done;
        if (!dst && want > sizeof scratch) want = sizeof scratch;
        int r = c.recv(p, want);
        if (r == -2) {
            if (!ss_read_exact(c, p, 1, 8000)) return false;   // stalled mid-frame: give it 8 s
            r = 1;
        } else if (r <= 0) return false;
        if (f.masked && dst) {
            const uint64_t base = offset + done;
            for (int i = 0; i < r; i++) p[i] ^= f.mask[(base + i) & 3];
        }
        done += (size_t)r;
    }
    return true;
}

// ---------------------------------------------------------------- listener
int ss_listen_tcp(uint16_t port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, backlog) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}
