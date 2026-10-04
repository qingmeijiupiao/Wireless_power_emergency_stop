#pragma once
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=0x101,ESP_ERR_INVALID_ARG=0x102,
 ESP_ERR_INVALID_STATE=0x103,ESP_ERR_NOT_FOUND=0x105,ESP_ERR_TIMEOUT=0x107,ESP_ERR_INVALID_CRC=0x109,ESP_ERR_INVALID_SIZE=0x104,ESP_ERR_NOT_SUPPORTED=0x106;
inline const char *esp_err_to_name(int){return "simulated-error";}
#define ESP_ERROR_CHECK(x) do {if((x)!=ESP_OK) throw 1;} while(0)
