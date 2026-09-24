// Minimal socket plumbing for the Second Screen network transports: a connection that is either
// a plain TCP socket or a TLS session, an HTTP request-head reader, and RFC 6455 WebSocket
// framing. Blocking I/O with per-socket timeouts; one task per connection.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct SsConn {
    int fd = -1;
    void *tls = nullptr;          // mbedtls_ssl_context* when TLS, else nullptr
    char peer_ip[16] = "";

    // >0 bytes read, 0 = peer closed, -1 = error, -2 = timeout (no data within the socket timeout)
    int  recv(void *buf, size_t n);
    // true when all n bytes were written
    bool send_all(const void *buf, size_t n);
    void set_timeout_ms(int ms);
    void close();
    bool valid() const { return fd >= 0; }
};

// Read exactly n bytes; false on close/error or when `deadline_ms` of total silence elapses.
bool ss_read_exact(SsConn &c, void *buf, size_t n, int deadline_ms);

// ---- HTTP -----------------------------------------------------------------------------------
struct SsHttpReq {
    char method[8] = "";
    char path[160] = "";          // without the query
    char query[160] = "";
    char host[64] = "";
    char ua[160] = "";
    char ws_key[40] = "";         // Sec-WebSocket-Key (upgrade requests)
    bool upgrade_ws = false;
};
// Read and parse a request head (up to 4 KB). false on timeout / garbage.
bool ss_http_read_head(SsConn &c, SsHttpReq &r, int timeout_ms);
bool ss_http_send(SsConn &c, int code, const char *ctype, const void *body, size_t len,
                  const char *extra_headers = nullptr);
bool ss_http_send_head(SsConn &c, int code, const char *ctype, size_t len,
                       const char *extra_headers = nullptr);
bool ss_query_get(const char *query, const char *key, char *out, size_t n);

// ---- WebSocket ------------------------------------------------------------------------------
enum SsWsOp : uint8_t { WS_CONT = 0, WS_TEXT = 1, WS_BIN = 2, WS_CLOSE = 8, WS_PING = 9, WS_PONG = 10 };

bool ss_ws_accept(SsConn &c, const SsHttpReq &r);             // send the 101 handshake
bool ss_ws_send(SsConn &c, SsWsOp op, const void *data, size_t len);
bool ss_ws_send_text(SsConn &c, const char *s);

// Frame header parsed off the wire (payload not yet read).
struct SsWsFrame {
    bool fin = false;
    uint8_t op = 0;
    bool masked = false;
    uint8_t mask[4] = {};
    uint64_t len = 0;
};
// Read a frame header. Returns 1 = got one, 0 = closed/error, -2 = idle timeout (no bytes at all).
int ss_ws_read_header(SsConn &c, SsWsFrame &f, int idle_ms);
// Read `n` payload bytes into dst, unmasking with the frame key; `offset` = bytes of this frame's
// payload already consumed (keeps the mask phase). dst may be null to discard.
bool ss_ws_read_payload(SsConn &c, const SsWsFrame &f, uint64_t offset, uint8_t *dst, size_t n);

// ---- listener helper ----
int ss_listen_tcp(uint16_t port, int backlog);
