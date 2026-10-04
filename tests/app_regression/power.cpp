// Production sleep entry: timeout permission, service quiescence and all rollback branches.
#include "power_manager.cpp"
namespace {
EmergencyRemote::Snapshot remote{};
bool remote_prepare_ok=true,battery_prepare_ok=true,display_shutdown_ok=true;
bool remote_paused=false,battery_paused=false;
int restored=0,shutdown_calls=0;
void prepare_display(){}
bool shutdown_display(){++shutdown_calls;return display_shutdown_ok;}
void restore_display(){++restored;}
const PowerManager::DisplayHooks hooks{prepare_display,shutdown_display,restore_display};
void reset(){
 Sim::now=1;Sim::usb=false;Sim::on_delay={};Sim::stop_at=0;remote={};
 remote.busy=false;remote.output_confirmed=true;remote.output_time_us=1;remote.quiesced=true;
 remote_prepare_ok=battery_prepare_ok=display_shutdown_ok=true;
 remote_paused=battery_paused=false;restored=shutdown_calls=0;Sim::isolated=0;Sim::wake_error=0;
}
}
namespace EmergencyRemote {
Snapshot snapshot(){return remote;}
bool prepare_sleep(){remote_paused=remote_prepare_ok;return remote_prepare_ok;}
void cancel_sleep(){remote_paused=false;}
}
namespace BatteryStatus {
bool get_status(BatteryLevel::Status&){return false;}
BatteryLevel::Status update(int voltage,bool){BatteryLevel::Status s{};s.voltage_mv=voltage;return s;}
}
namespace BatteryVoltage {
bool prepare_sleep(TickType_t){battery_paused=true;return battery_prepare_ok;}
void cancel_sleep(){battery_paused=false;}
esp_err_t wait_mv(int&,TickType_t){return ESP_ERR_INVALID_STATE;}
}
#include "settings_fake.h"
int main(){
 reset();remote.output_confirmed=false;remote.output_time_us=0;
 assert(PowerManager::block()==PowerManager::SleepBlock::Unknown);
 remote.output_on=true;remote.stop_timed_out=true;remote.connection_failed=true;
 assert(PowerManager::block()==PowerManager::SleepBlock::None);
 remote.busy=true;assert(PowerManager::block()!=PowerManager::SleepBlock::None);
 puts("PASS power: only explicit close timeout bypasses unknown/possible ON, never pending work");

 reset();battery_prepare_ok=false;
 assert(PowerManager::enter_sleep(hooks,true)==4);
 assert(!battery_paused&&!remote_paused&&Sim::isolated==0&&shutdown_calls==0);
 reset();Sim::wake_error=ESP_FAIL;
 assert(PowerManager::enter_sleep(hooks,true)==7);
 assert(!battery_paused&&!remote_paused&&Sim::isolated==0);
 reset();display_shutdown_ok=false;
 assert(PowerManager::enter_sleep(hooks,true)==4);
 assert(!battery_paused&&!remote_paused&&Sim::isolated==0&&restored==1);
 puts("PASS power: battery/display/wake configuration failures abort before GPIO isolation and resume services");

 reset();assert(PowerManager::enter_sleep(hooks,true)==4);
 assert(!battery_paused&&!remote_paused&&restored==1&&Sim::isolated>0);
 puts("PASS power: rejected deep sleep resumes every paused service and restores display");
}
