#pragma once
#include "esp_err.h"
#include "simulation.h"
#define CONFIG_ESTOP_OLED_SDA 1
#define CONFIG_ESTOP_OLED_SCL 2
#define CONFIG_ESTOP_OLED_COLUMN_OFFSET 2
struct Sh1106{unsigned addr=0;unsigned address(){return addr;}
 int init(int,int,int){++Sim::oled_inits;addr=0x3c;return ESP_OK;}
 int write_frame(const void*,size_t){++Sim::oled_writes;return Sim::oled_error;}
 void shutdown(){addr=0;}};
