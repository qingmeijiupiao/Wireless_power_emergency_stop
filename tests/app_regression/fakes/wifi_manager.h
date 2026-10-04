#pragma once
#include "esp_err.h"
#include "simulation.h"
class WiFiManager {public:static WiFiManager& instance(){static WiFiManager m;return m;}
 int init(){return ESP_OK;}int set_channel(int){return ESP_OK;}
 int start_sta_radio(int){Sim::radio=true;return ESP_OK;}int stop(){Sim::radio=false;return ESP_OK;}};
