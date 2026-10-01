#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
typedef struct esp_http_client *esp_http_client_handle_t;
typedef enum { HTTP_EVENT_ERROR, HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADERS_SENT, HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_FINISH, HTTP_EVENT_DISCONNECTED } esp_http_client_event_id_t;
typedef struct { esp_http_client_event_id_t event_id; esp_http_client_handle_t client; void *data; int data_len; void *user_data; char *header_key; char *header_value; } esp_http_client_event_t;
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST } esp_http_client_method_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t*);
typedef struct { const char *url; const char *host; const char *path; esp_http_client_method_t method; int timeout_ms; bool disable_auto_redirect; int max_redirection_count; http_event_handle_cb event_handler; void *user_data; int buffer_size; int buffer_size_tx; esp_err_t (*crt_bundle_attach)(void*); const char *user_agent; bool keep_alive_enable; bool skip_cert_common_name_check; const char *cert_pem; } esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*);
esp_err_t esp_http_client_perform(esp_http_client_handle_t); esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t); int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t); esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_read(esp_http_client_handle_t, char*, int); int esp_http_client_write(esp_http_client_handle_t, const char*, int);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char*, const char*);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char*, int);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t, esp_http_client_method_t);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t);
