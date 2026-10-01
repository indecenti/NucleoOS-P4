#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef enum { ESP_PARTITION_TYPE_APP, ESP_PARTITION_TYPE_DATA } esp_partition_type_t;
typedef enum { ESP_PARTITION_SUBTYPE_ANY = 0xff } esp_partition_subtype_t;
typedef enum { ESP_PARTITION_MMAP_DATA, ESP_PARTITION_MMAP_INST } esp_partition_mmap_memory_t;
typedef uint32_t esp_partition_mmap_handle_t;
typedef struct { esp_partition_type_t type; int subtype; uint32_t address; uint32_t size; char label[17]; } esp_partition_t;
const esp_partition_t *esp_partition_find_first(esp_partition_type_t, int, const char*);
esp_err_t esp_partition_mmap(const esp_partition_t*, size_t, size_t, esp_partition_mmap_memory_t, const void**, esp_partition_mmap_handle_t*);
void esp_partition_munmap(esp_partition_mmap_handle_t);
