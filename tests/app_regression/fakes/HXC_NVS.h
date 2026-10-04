#pragma once
#include "esp_err.h"
#include "simulation.h"
#include <type_traits>
namespace HXC {struct NVS_Base {static int setup(){return ESP_OK;}};
template<class T>class NVS_DATA:public NVS_Base {std::string key;T value;
 public:NVS_DATA(const char*k,T v):key(k),value(v){}T read(){return value;}
 int set(const T&v){++Sim::persist_calls;if(Sim::nvs_result!=ESP_OK)return Sim::nvs_result;
 if constexpr(std::is_same_v<T,uint32_t>){Sim::disk[key]=v;if(Sim::after_persist){auto fn=Sim::after_persist;fn(key);}}
 value=v;return ESP_OK;}};}
