#pragma once
#include "simulation.h"
#define ESP_LOGI(tag,...) Sim::log(__VA_ARGS__)
#define ESP_LOGW(tag,...) Sim::log(__VA_ARGS__)
#define ESP_LOGE(tag,...) Sim::log(__VA_ARGS__)

enum esp_log_level_t {ESP_LOG_NONE,ESP_LOG_ERROR,ESP_LOG_WARN,ESP_LOG_INFO,ESP_LOG_DEBUG};
#define ESP_LOG_LEVEL(level,tag,...) Sim::log(__VA_ARGS__)
