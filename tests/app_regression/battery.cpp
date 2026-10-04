// Fixed-pool identities, handoff races, sleep gating and calibration failures use production code.
#include "battery_voltage.cpp"
using namespace BatteryVoltage;
int main(){
 assert(init()==ESP_OK);
 assert(Sim::tasks.count("battery_sample")==1);
 assert(start_async()==ESP_OK);
 SampleRequest* first=latest_request;
 ++first->references; // Simulate wait_mv retaining the request before completion.
 Sim::hook_sem=state_mutex;
 Sim::on_give=[] {if(!sampling){Sim::on_give={};Sim::adc_mv=2000;assert(start_async()==ESP_OK);}};
 perform_sample(first);
 SampleRequest* second=latest_request;
 assert(first!=second && sampling);
 int voltage=0;
 assert(wait_mv(voltage,0)==ESP_ERR_TIMEOUT); // A's notification cannot wake B.
 perform_sample(second);
 assert(await_sample(first,voltage,0)==ESP_OK && voltage==4200);
 assert(wait_mv(voltage,0)==ESP_OK && voltage==4000);
 puts("PASS battery: completion and result remain bound to each request across handoff");

 Sim::on_give={};Sim::hook_sem=nullptr;
 assert(start_async()==ESP_OK);
 assert(!prepare_sleep(0));assert(start_async()==ESP_ERR_INVALID_STATE);
 perform_sample(latest_request);assert(prepare_sleep(0));assert(start_async()==ESP_ERR_INVALID_STATE);
 cancel_sleep();assert(start_async()==ESP_OK);perform_sample(latest_request);
 puts("PASS battery: sleep gate blocks submissions until current GPIO/callback work finishes");

 Sim::on_wait=[] {if(sampling)perform_sample(latest_request);};
 Sim::adc_mv=2100;Sim::nvs_result=ESP_FAIL;Sim::usb=true;Sim::logs.clear();Sim::persist_calls=0;
 assert(start_calibration_monitor([] {return true;})==ESP_OK);
 calibration_monitor_task(nullptr);
 assert(Sim::persist_calls==3 && !stored_calibration_valid && !calibration_monitor_running);
 for(const auto& message:Sim::logs)assert(message.find("calibration complete")==std::string::npos);
 CalibrationStatus status{};get_calibration_status(status);assert(status.last_error==ESP_FAIL);
 puts("PASS battery: failed NVS calibration never reports success, bounded to three full-window attempts");

 Sim::nvs_result=ESP_OK;Sim::persist_calls=0;Sim::stop_at=0;
 bool did_reset=false;int samples_after_reset=0;
 assert(start_calibration_monitor([]() -> bool {return true;})==ESP_OK); // callback below replaced without task race
 usb_connected_callback=[] {return true;};
 Sim::on_delay=[&]{
  if(!did_reset && calibration_stable_samples==50){assert(reset_calibration()==ESP_OK);did_reset=true;}
 };
 Sim::on_wait=[&]{if(sampling){perform_sample(latest_request);if(did_reset)++samples_after_reset;}};
 calibration_monitor_task(nullptr);
 assert(did_reset && samples_after_reset>=60 && Sim::persist_calls==2 && stored_calibration_valid);
 puts("PASS battery: reset discards the old calibration window and requires 60 new samples");

 Sim::on_delay={};const auto epoch=current_calibration_epoch();
 notify_external_power(true);notify_external_power(false);notify_external_power(true);
 assert(current_calibration_epoch()>=epoch+2);
 assert(save_calibration(DEFAULT_DIVIDER_SCALE_Q16,epoch)==ESP_ERR_INVALID_STATE);
 puts("PASS battery: USB session changes invalidate old save transactions");
}
