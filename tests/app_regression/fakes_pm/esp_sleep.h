#pragma once
#include "esp_err.h"
#include "hardware.h"
constexpr int ESP_SLEEP_WAKEUP_GPIO=4,ESP_SLEEP_WAKEUP_ALL=0,ESP_GPIO_WAKEUP_GPIO_LOW=0,ESP_GPIO_WAKEUP_GPIO_HIGH=1;
inline unsigned esp_sleep_get_wakeup_causes(){return 0;}
inline void esp_sleep_disable_wakeup_source(int){}
inline int esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(uint64_t,int){return Sim::wake_error;}
inline int esp_deep_sleep_try_to_start(){return ESP_FAIL;}
