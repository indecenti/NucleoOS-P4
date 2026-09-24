// TLS for the NucleoCast https listener: a per-device self-signed ECDSA P-256 certificate,
// generated on first use and kept on the SD card so the browser's one-time exception survives
// reboots. The point of https here is not trust but a SECURE CONTEXT: browsers only expose
// getDisplayMedia() (screen capture) to https / localhost / file pages.
#include "ss_tls.h"

#include "nv_log.h"

#include "mbedtls/ssl.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace {

constexpr const char *TAG = "ss_tls";
constexpr const char *kDir = "/sdcard/nucleos/ss";
constexpr const char *kKeyPath = "/sdcard/nucleos/ss/tls_key.pem";
constexpr const char *kCrtPath = "/sdcard/nucleos/ss/tls_crt.pem";

bool s_ready = false;
mbedtls_ssl_config s_conf;
mbedtls_x509_crt s_crt;
mbedtls_pk_context s_key;

int rng(void *, unsigned char *out, size_t n) {
    esp_fill_random(out, n);
    return 0;
}

bool read_file(const char *path, unsigned char **buf, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 8192) { fclose(f); return false; }
    *buf = (unsigned char *)calloc(1, (size_t)n + 1);   // PEM parsers want the NUL
    if (!*buf) { fclose(f); return false; }
    *len = fread(*buf, 1, (size_t)n, f);
    fclose(f);
    (*buf)[*len] = 0;
    *len += 1;
    return true;
}

bool write_file(const char *path, const unsigned char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

bool load(void) {
    unsigned char *k = nullptr, *c = nullptr;
    size_t kl = 0, cl = 0;
    bool ok = read_file(kKeyPath, &k, &kl) && read_file(kCrtPath, &c, &cl) &&
              mbedtls_pk_parse_key(&s_key, k, kl, nullptr, 0, rng, nullptr) == 0 &&
              mbedtls_x509_crt_parse(&s_crt, c, cl) == 0;
    free(k);
    free(c);
    return ok;
}

// Generate key + self-signed cert (CN/SAN nucleov2.local, valid to 2049). Keeps them loaded.
bool generate(void) {
    mbedtls_pk_free(&s_key);
    mbedtls_pk_init(&s_key);
    if (mbedtls_pk_setup(&s_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) return false;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(s_key), rng, nullptr) != 0) return false;

    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, &s_key);
    mbedtls_x509write_crt_set_issuer_key(&w, &s_key);
    bool ok = mbedtls_x509write_crt_set_subject_name(&w, "CN=nucleov2.local,O=NucleoOS,OU=Second Screen") == 0 &&
              mbedtls_x509write_crt_set_issuer_name(&w, "CN=nucleov2.local,O=NucleoOS,OU=Second Screen") == 0 &&
              mbedtls_x509write_crt_set_validity(&w, "20250101000000", "20491231235959") == 0 &&
              mbedtls_x509write_crt_set_basic_constraints(&w, 0, -1) == 0;
    unsigned char serial[8];
    esp_fill_random(serial, sizeof serial);
    serial[0] &= 0x7F;   // positive
    ok = ok && mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial) == 0;
    mbedtls_x509_san_list san = {};
    san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san.node.san.unstructured_name.p = (unsigned char *)"nucleov2.local";
    san.node.san.unstructured_name.len = strlen("nucleov2.local");
    ok = ok && mbedtls_x509write_crt_set_subject_alternative_name(&w, &san) == 0;

    constexpr size_t kBuf = 2048;
    unsigned char *crt_pem = (unsigned char *)calloc(1, kBuf);
    unsigned char *key_pem = (unsigned char *)calloc(1, kBuf);
    ok = ok && crt_pem && key_pem &&
         mbedtls_x509write_crt_pem(&w, crt_pem, kBuf, rng, nullptr) == 0 &&
         mbedtls_pk_write_key_pem(&s_key, key_pem, kBuf) == 0;
    mbedtls_x509write_crt_free(&w);
    if (ok) {
        mbedtls_x509_crt_free(&s_crt);
        mbedtls_x509_crt_init(&s_crt);
        ok = mbedtls_x509_crt_parse(&s_crt, crt_pem, strlen((char *)crt_pem) + 1) == 0;
    }
    if (ok) {
        mkdir("/sdcard/nucleos", 0777);
        mkdir(kDir, 0777);
        if (!write_file(kKeyPath, key_pem, strlen((char *)key_pem)) ||
            !write_file(kCrtPath, crt_pem, strlen((char *)crt_pem)))
            NV_LOGW(TAG, "certificate not saved to SD (kept in RAM for this boot)");
    }
    free(crt_pem);
    free(key_pem);
    return ok;
}

int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    const int fd = (int)(intptr_t)ctx;   // fd by value: the SsConn is copied between tasks
    int r = send(fd, buf, len, 0);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return r;
}

int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    const int fd = (int)(intptr_t)ctx;
    int r = recv(fd, buf, len, 0);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    if (r == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return r;
}

}  // namespace

bool ss_tls_init(void) {
    if (s_ready) return true;
    mbedtls_x509_crt_init(&s_crt);
    mbedtls_pk_init(&s_key);
    if (!load()) {
        mbedtls_x509_crt_free(&s_crt); mbedtls_x509_crt_init(&s_crt);
        mbedtls_pk_free(&s_key); mbedtls_pk_init(&s_key);
        NV_LOGI(TAG, "generating the device certificate (ECDSA P-256)");
        if (!generate()) { NV_LOGE(TAG, "certificate generation failed"); return false; }
    }
    mbedtls_ssl_config_init(&s_conf);
    if (mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        mbedtls_ssl_conf_own_cert(&s_conf, &s_crt, &s_key) != 0) {
        NV_LOGE(TAG, "TLS config failed");
        return false;
    }
    mbedtls_ssl_conf_rng(&s_conf, rng, nullptr);
    s_ready = true;
    NV_LOGI(TAG, "https ready");
    return true;
}

bool ss_tls_ready(void) { return s_ready; }

bool ss_tls_wrap(SsConn &c, int timeout_ms) {
    if (!s_ready || c.fd < 0) return false;
    auto *ssl = (mbedtls_ssl_context *)calloc(1, sizeof(mbedtls_ssl_context));
    if (!ssl) return false;
    mbedtls_ssl_init(ssl);
    if (mbedtls_ssl_setup(ssl, &s_conf) != 0) { mbedtls_ssl_free(ssl); free(ssl); return false; }
    mbedtls_ssl_set_bio(ssl, (void *)(intptr_t)c.fd, bio_send, bio_recv, nullptr);
    c.set_timeout_ms(timeout_ms);
    int r;
    int spins = 0;
    while ((r = mbedtls_ssl_handshake(ssl)) != 0) {
        if ((r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) && ++spins < 3) continue;
        // -0x7780 = the browser rejected our certificate on this connection (expected until the user
        // accepts the warning once): not worth an error line.
        if (r != MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) NV_LOGD(TAG, "handshake failed: -0x%04x", -r);
        mbedtls_ssl_free(ssl);
        free(ssl);
        return false;
    }
    c.tls = ssl;
    return true;
}
