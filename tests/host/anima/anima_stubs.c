// Host stand-ins for the ESP-IDF / NucleoOS services the ANIMA engine calls. Everything network-side
// fails cleanly (the device is "offline"), the clock and heap are real, the sealed vault is a plain
// file, FreeRTOS locks are single-threaded no-ops (the tests drive one query at a time).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "mdns.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
int64_t esp_timer_get_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

void *heap_caps_malloc(size_t n, uint32_t c) { (void)c; return malloc(n); }
void *heap_caps_calloc(size_t a, size_t b, uint32_t c) { (void)c; return calloc(a, b); }
void *heap_caps_realloc(void *p, size_t n, uint32_t c) { (void)c; return realloc(p, n); }
void heap_caps_free(void *p) { free(p); }
size_t heap_caps_get_free_size(uint32_t c) { (void)c; return 8u << 20; }
size_t heap_caps_get_largest_free_block(uint32_t c) { (void)c; return 4u << 20; }
size_t heap_caps_get_minimum_free_size(uint32_t c) { (void)c; return 4u << 20; }

esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
esp_err_t esp_task_wdt_status(TaskHandle_t t) { (void)t; return ESP_FAIL; }
esp_err_t esp_crt_bundle_attach(void *c) { (void)c; return ESP_OK; }

const esp_partition_t *esp_partition_find_first(esp_partition_type_t t, int s, const char *l) { (void)t; (void)s; (void)l; return NULL; }
esp_err_t esp_partition_mmap(const esp_partition_t *p, size_t o, size_t s, esp_partition_mmap_memory_t m, const void **out, esp_partition_mmap_handle_t *h)
{ (void)p; (void)o; (void)s; (void)m; (void)out; (void)h; return ESP_FAIL; }
void esp_partition_munmap(esp_partition_mmap_handle_t h) { (void)h; }

// Offline: no HTTP client can be created, so every online tier takes its failure path.
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) { (void)c; return NULL; }
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) { (void)c; return ESP_FAIL; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c; return -1; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return 0; }
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int l) { (void)c; (void)l; return ESP_FAIL; }
int esp_http_client_read(esp_http_client_handle_t c, char *b, int l) { (void)c; (void)b; (void)l; return -1; }
int esp_http_client_write(esp_http_client_handle_t c, const char *b, int l) { (void)c; (void)b; (void)l; return -1; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v) { (void)c; (void)k; (void)v; return ESP_OK; }
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *d, int l) { (void)c; (void)d; (void)l; return ESP_OK; }

esp_err_t mdns_init(void) { return ESP_OK; }
esp_err_t mdns_query_ptr(const char *s, const char *p, uint32_t t, size_t m, mdns_result_t **r) { (void)s; (void)p; (void)t; (void)m; *r = NULL; return ESP_FAIL; }
void mdns_query_results_free(mdns_result_t *r) { (void)r; }

void mbedtls_sha256_init(mbedtls_sha256_context *c) { memset(c, 0, sizeof *c); }
void mbedtls_sha256_free(mbedtls_sha256_context *c) { (void)c; }
int mbedtls_sha256_starts(mbedtls_sha256_context *c, int i) { (void)c; (void)i; return 0; }
int mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *d, size_t n) { (void)c; (void)d; (void)n; return 0; }
int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char *o) { (void)c; memset(o, 0, 32); return 0; }

void vTaskDelay(TickType_t t) { (void)t; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(esp_timer_get_time() / 1000); }
static int s_sem;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_sem; }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return &s_sem; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }

// NucleoOS services
const char *nucleo_setup_ip(void) { return ""; }        // not associated: offline
char *nv_sealed_read(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    char *b = malloc(max + 1); size_t n = b ? fread(b, 1, max, f) : 0; fclose(f);
    if (!b) return NULL;
    b[n] = 0; if (len) *len = n; return b;
}
bool nv_sealed_write(const char *path, const void *d, size_t n)
{
    FILE *f = fopen(path, "wb"); if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n; return fclose(f) == 0 && ok;
}
