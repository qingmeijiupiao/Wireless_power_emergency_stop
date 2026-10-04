#pragma once
#include "esp_err.h"
#include <cstdint>
constexpr int WIFI_IF_STA=0;
struct esp_now_peer_info_t{uint8_t peer_addr[6],lmk[16],channel;int ifidx;bool encrypt;};
inline bool esp_now_is_peer_exist(const uint8_t*){return true;}
inline int esp_now_mod_peer(const esp_now_peer_info_t*){return 0;}
inline int esp_now_add_peer(const esp_now_peer_info_t*){return 0;}
inline int esp_now_del_peer(const uint8_t*){return 0;}
extern int physical_sends;
inline int esp_now_send(const uint8_t*,const uint8_t*,size_t){++physical_sends;return 0;}
