#pragma once
#include <cstdint>
using TickType_t=uint32_t;
using BaseType_t=int;
using TaskHandle_t=void*;
using portMUX_TYPE=int;
constexpr int pdTRUE=1,pdFALSE=0,pdPASS=1,portMUX_INITIALIZER_UNLOCKED=0,configTICK_RATE_HZ=1000;
constexpr TickType_t portMAX_DELAY=0xffffffffU;
#define pdMS_TO_TICKS(x) (x)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))

#define portENTER_CRITICAL_ISR(x) ((void)(x))
#define portEXIT_CRITICAL_ISR(x) ((void)(x))
constexpr int portTICK_PERIOD_MS=1;
using UBaseType_t=unsigned;
