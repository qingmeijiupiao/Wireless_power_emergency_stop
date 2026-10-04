// Real writer mutex with two interleaved writers, compiling production RuntimeSettings.
#include "runtime_settings.h"
#include "esp_err.h"
#include "simulation.h"
int main(){
 RuntimeSettings::init();std::atomic_bool started=false,finished=false;
 std::thread writer;
 Sim::after_persist=[&](const std::string&key){
  if(key=="idle_ms" && !started){
   writer=std::thread([&]{started=true;assert(RuntimeSettings::set("idle_ms",1800000));finished=true;});
   while(!started)std::this_thread::yield();
   std::this_thread::sleep_for(std::chrono::milliseconds(30));assert(!finished);
  }
 };
 assert(RuntimeSettings::set("idle_ms",600000));writer.join();
 assert(Sim::disk["idle_ms"]==1800000 && RuntimeSettings::get(RuntimeSettings::Id::IdleMs)==1800000);
 Sim::nvs_result=ESP_FAIL;assert(!RuntimeSettings::set_always_on(true));assert(!RuntimeSettings::always_on());
 assert(!RuntimeSettings::set("idle_ms",1));assert(RuntimeSettings::get(nullptr)==0);
 puts("PASS settings: serialized persist/publish, failure rollback, range checks");
}
