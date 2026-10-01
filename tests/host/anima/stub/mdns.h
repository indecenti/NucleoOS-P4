#pragma once
#include <stdint.h>
#include "esp_err.h"
#define ESP_IPADDR_TYPE_V4 0
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr & 0xff),(int)(((a)->addr>>8)&0xff),(int)(((a)->addr>>16)&0xff),(int)(((a)->addr>>24)&0xff)
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { union { esp_ip4_addr_t ip4; } u_addr; uint8_t type; } esp_ip_addr_t;
typedef struct mdns_ip_addr_s { struct mdns_ip_addr_s *next; esp_ip_addr_t addr; } mdns_ip_addr_t;
typedef struct mdns_result_s { struct mdns_result_s *next; uint16_t port; mdns_ip_addr_t *addr; char *hostname; char *instance_name; } mdns_result_t;
esp_err_t mdns_init(void); esp_err_t mdns_query_ptr(const char*, const char*, uint32_t, size_t, mdns_result_t**); void mdns_query_results_free(mdns_result_t*);
