#pragma once
#include "esp_err.h"
#include "simulation.h"
struct esp_partition_t {uint32_t size=12288;};
constexpr int ESP_PARTITION_TYPE_DATA=1,ESP_PARTITION_SUBTYPE_ANY=0;
inline uint8_t flash_memory[12288];
inline const esp_partition_t*esp_partition_find_first(int,int,const char*){static esp_partition_t p;return &p;}
inline int esp_partition_read(const esp_partition_t*,size_t offset,void*data,size_t size){
 if(offset+size>sizeof(flash_memory))return ESP_FAIL;memcpy(data,flash_memory+offset,size);return ESP_OK;}
inline int esp_partition_write(const esp_partition_t*,size_t offset,const void*data,size_t size){
 if(offset+size>sizeof(flash_memory))return ESP_FAIL;memcpy(flash_memory+offset,data,size);return ESP_OK;}
inline int esp_partition_erase_range(const esp_partition_t*,size_t offset,size_t size){
 if(offset+size>sizeof(flash_memory))return ESP_FAIL;memset(flash_memory+offset,255,size);return ESP_OK;}
