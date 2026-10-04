#pragma once
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
namespace Sim {
struct Stop{};
struct Semaphore{bool mutex;int count;std::mutex gate;std::condition_variable ready;};
struct Queue{unsigned capacity;size_t size;std::deque<std::vector<unsigned char>>items;};
inline int64_t now=1,stop_at=0;
inline bool stop_closed=true,usb=false,radio=true,immediate_tasks=false;
inline int nvs_result=0,persist_calls=0,oled_inits=0,oled_writes=0;
inline Semaphore*hook_sem=nullptr;
inline std::function<void()>on_delay,on_give,on_wait;
inline std::map<std::string,void(*)(void*)> tasks;
inline int adc_mv=2100,adc_error=0,oled_error=-1;
inline std::mutex log_mutex;
inline std::function<void(const std::string&)>after_persist;
inline std::map<std::string,uint32_t>disk;
inline std::map<std::string,uint32_t>setting_overrides;
inline std::vector<std::string>logs;
inline void log(const char*format,...){char text[512];va_list args;va_start(args,format);
 std::vsnprintf(text,sizeof(text),format,args);va_end(args);std::lock_guard<std::mutex> guard(log_mutex);logs.emplace_back(text);}
inline uint32_t setting(const char *key){
 if(auto entry=setting_overrides.find(key);entry!=setting_overrides.end())return entry->second;
 if(!std::strcmp(key,"connect_ms"))return 1000;
 if(!std::strcmp(key,"off_ack_ms"))return 500;
 if(!std::strcmp(key,"off_retry_ms"))return 1000;
 if(!std::strcmp(key,"on_ack_ms"))return 500;
 if(!std::strcmp(key,"fresh_ms"))return 3000;
 if(!std::strcmp(key,"release_ms"))return 100;
 if(!std::strcmp(key,"low_mv"))return 3500;
 if(!std::strcmp(key,"notice_ms"))return 30000;
 return 300000;
}
}
