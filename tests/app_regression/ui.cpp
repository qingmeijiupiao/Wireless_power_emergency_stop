// Page policy uses production UiManager; OLED actor runs independently under virtual time.
#include "emergency_ui.h"
#include "core/ui_manager.h"
#include "core/button_input.h"
#include "core/display_worker.cpp"
#include "battery_status.h"
#include "settings_fake.h"
#include "product_ui.h"
namespace BatteryStatus{bool get_status(BatteryLevel::Status&){return false;}}
namespace EmergencyRemote{Snapshot snapshot(){return {};}}
namespace EmergencyUi::Buttons{esp_err_t init(){return ESP_OK;}uint32_t dropped(){return 0;}bool poll(Gesture&){return false;}}
int main(){
 auto&ui=EmergencyUi::UiManager::instance();ui.reset(Sim::now);
 EmergencyUi::Model model;model.remote.state=EmergencyRemote::State::OFF;
 model.remote.busy=false;model.remote.online=true;model.battery_mv=3400;model.now_us=Sim::now;
 ui.handle_input(model.remote,EmergencyUi::Gesture::None,model.now_us);ui.observe_state(model);
 assert(ui.page_key(model)==304);
 Sim::now+=240000000;model.now_us=Sim::now;
 ui.handle_input(model.remote,EmergencyUi::Gesture::None,model.now_us);ui.observe_state(model);
 PowerManager::SleepCountdown countdown;model.sleep=countdown.update(model.now_us,300000001,true);
 assert(model.sleep.countdown && ui.sleep_notice_allowed(model.now_us) && ui.page_key(model)==500);
 ui.show_message(4,model.now_us,false);assert(ui.page_key(model)==500);
 model.remote.state=EmergencyRemote::State::STOPPING;assert(ui.page_key(model)!=500);
 puts("PASS UI: expired low notice exits; countdown overrides ordinary messages, STOP remains priority");
 uint8_t positive[1024],negative[1024];
 for(int current: {0,1,1234567,98765432,2147483647}) {
  auto measurement=model.remote;measurement.online=true;measurement.data.current_ua=current;
  ProductUi::render_home(positive,measurement);measurement.data.current_ua=-current;
  ProductUi::render_home(negative,measurement);
  assert(memcmp(positive,negative,1024)==0 && measurement.data.current_ua==-current);
 }
 puts("PASS UI: screen current magnitude is sign-independent and leaves raw measurements unchanged");

 model.remote.state=EmergencyRemote::State::OFF;EmergencyUi::init();EmergencyUi::restore();
 const auto initial_time=Sim::now;
 for(int i=0;i<20;++i){model.now_us=Sim::now;EmergencyUi::render(model,true);}
 assert(Sim::now==initial_time && Sim::oled_writes==0);
 Sim::stop_at=Sim::now+7000000;
 try{EmergencyUi::DisplayWorker::worker(nullptr);}catch(const Sim::Stop&){}
 assert(Sim::oled_inits>=2 && Sim::oled_writes<12);
 puts("PASS OLED: foreground never waits on I2C; failed writes back off and rebuild bus");

 Sim::stop_at=0;Sim::oled_error=ESP_OK;Sim::oled_writes=0;
 uint8_t pixels[1024]{};assert(EmergencyUi::DisplayWorker::submit(pixels));
 Sim::on_delay=[] { // Confirm shutdown processing with a scheduled worker; stop after acknowledgement.
  if(!EmergencyUi::DisplayWorker::accepting && EmergencyUi::DisplayWorker::completed==EmergencyUi::DisplayWorker::serial)
   throw Sim::Stop{};
 };
 Sim::on_wait={};
 // No stack pointers are queued: timeout is safe and rejects sleep; worker can finish later.
 Sim::on_delay={};assert(!EmergencyUi::DisplayWorker::shutdown());
 assert(!EmergencyUi::DisplayWorker::submit(pixels));
 Sim::stop_at=Sim::now+30000;
 try{EmergencyUi::DisplayWorker::worker(nullptr);}catch(const Sim::Stop&){}
 assert(EmergencyUi::DisplayWorker::completed==EmergencyUi::DisplayWorker::serial);
 const int writes=Sim::oled_writes;
 Sim::stop_at=Sim::now+100000;
 try{EmergencyUi::DisplayWorker::worker(nullptr);}catch(const Sim::Stop&){}
 assert(Sim::oled_writes==writes);
 puts("PASS OLED: shutdown timeout keeps submissions gated; acknowledged pause forbids subsequent writes");
}
