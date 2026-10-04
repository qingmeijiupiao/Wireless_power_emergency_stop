#pragma once
#include <cstdint>
#include "esp_err.h"
using gpio_num_t=int;
constexpr int GPIO_NUM_10=10,GPIO_MODE_INPUT=1,GPIO_MODE_OUTPUT_OD=2,GPIO_PULLUP_DISABLE=0,
 GPIO_PULLDOWN_DISABLE=0,GPIO_INTR_DISABLE=0;
struct gpio_config_t{uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type;};
inline int gpio_config(const gpio_config_t*){return ESP_OK;}inline int gpio_set_level(int,int){return ESP_OK;}
