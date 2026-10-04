#pragma once
#define BUILD_TIME "simulation"
struct esp_app_desc_t{const char*version="simulation";};
inline const esp_app_desc_t *esp_app_get_description(){static esp_app_desc_t d;return &d;}
