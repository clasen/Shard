#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t; typedef int esp_partition_mmap_handle_t;
#define ESP_OK 0
enum { ESP_PARTITION_TYPE_DATA = 1 }; typedef int esp_partition_subtype_t; enum { ESP_PARTITION_MMAP_DATA = 0 };
struct esp_partition_t { uint32_t size; };
static const esp_partition_t* esp_partition_find_first(int, esp_partition_subtype_t, const char*) { return nullptr; }
static esp_err_t esp_partition_read(const esp_partition_t*, size_t, void*, size_t) { return 1; }
static esp_err_t esp_partition_mmap(const esp_partition_t*, size_t, size_t, int, const void**, esp_partition_mmap_handle_t*) { return 1; }
