#pragma once
#include "esp_err.h"
#include "simulation.h"
using adc_channel_t=int;
constexpr int ADC_CHANNEL_0=0;
struct adc_t{explicit adc_t(int){}int init(){return ESP_OK;}
 int read_voltage_mV(int&v){v=Sim::adc_mv;return Sim::adc_error;}};
