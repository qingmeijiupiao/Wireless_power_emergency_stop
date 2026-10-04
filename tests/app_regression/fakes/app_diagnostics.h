#pragma once
#include "esp_log.h"
#define APP_LOGI(tag,...) Sim::log(__VA_ARGS__)
#define APP_LOGW(tag,...) Sim::log(__VA_ARGS__)
#define APP_LOGE(tag,...) Sim::log(__VA_ARGS__)
