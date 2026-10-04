// Compile the actual worker; advance virtual time and inject callback/ISR events at boundaries.
#include "emergency_remote.cpp"
#include "sleep_policy.h"
#include "settings_fake.h"
namespace {
EspNowLink::MacAddress peer{{1,2,3,4,5,6}};
uint32_t request_id=0;
bool ack_off=false,ack_on=false,send_protection=false,submit_on_error=false;
int cancellations=0,on_submits=0,off_submits=0;
}
namespace EspNowLink {
bool MacAddress::operator==(const MacAddress&o)const{return std::memcmp(bytes,o.bytes,6)==0;}
bool MacAddress::operator!=(const MacAddress&o)const{return !(*this==o);}
bool is_active(){return Sim::radio;}
bool is_pairing(){return false;}
bool is_recovering_channel(){return false;}
size_t get_saved_peer_count(){return 1;}
esp_err_t get_saved_peer(size_t,SavedPeer*p){*p={peer,1};return ESP_OK;}
esp_err_t remove_peer(const MacAddress&){return ESP_OK;}
esp_err_t remove_saved_peer(const MacAddress&){return ESP_FAIL;}
esp_err_t start_pairing(){return ESP_OK;}
esp_err_t recover_peer_channel(const MacAddress&){return ESP_OK;}
void leave_pairing_mode(){}
void get_statistics(LinkStatistics*p){*p={};}
esp_err_t register_handler(uint16_t,MessageHandler,void*){return ESP_OK;}
void cancel_transmissions(){++cancellations;}
void cancel_transmissions_from_isr(){++cancellations;}
}
namespace EspNowService {
esp_err_t init(){return ESP_OK;}
void set_switch_response_handler(SwitchResponseHandler,void*){}
void set_data_received_handler(DataReceivedHandler,void*){}
esp_err_t send_remote_battery(const EspNowLink::MacAddress&,uint8_t,EspNowLink::SendCallback,void*){return ESP_OK;}
esp_err_t send_switch_request(const EspNowLink::MacAddress&,SwitchAction action,uint32_t*id,EspNowLink::SendCallback,void*){
 *id=++request_id;
 if(action==SwitchAction::ON){++on_submits;if(submit_on_error){submit_on_error=false;return ESP_ERR_NO_MEM;}}
 else ++off_submits;
 if((ack_off && action==SwitchAction::OFF)||(ack_on && action==SwitchAction::ON))
  EmergencyRemote::receive_switch(peer,*id,action,SwitchResult::OK,action==SwitchAction::ON,nullptr);
 return ESP_OK;
}
esp_err_t request_device_data(const EspNowLink::MacAddress&,uint32_t*id,EspNowLink::SendCallback,void*){
 *id=++request_id; DeviceData data{};data.status_flags=send_protection?2:0;
 EmergencyRemote::receive_data(peer,*id,data,true,false,nullptr);return ESP_OK;
}
}
void reset(){
 Sim::now=1;Sim::radio=true;Sim::stop_closed=true;Sim::on_delay={};
 Sim::setting_overrides.clear();
 EmergencyRemote::model={};EmergencyRemote::controller=peer;
 if(EmergencyRemote::responses)vQueueDelete(EmergencyRemote::responses);
 if(EmergencyRemote::telemetry)vQueueDelete(EmergencyRemote::telemetry);
 EmergencyRemote::responses=xQueueCreate(16,sizeof(EmergencyRemote::Response));
 EmergencyRemote::telemetry=xQueueCreate(1,sizeof(EmergencyRemote::Telemetry));
 EmergencyRemote::pause_requested=false;EmergencyRemote::contact_fell=false;
 EmergencyRemote::stop_requested=false;EmergencyRemote::on_requested=false;
 EmergencyRemote::pair_requested=false;EmergencyRemote::retry_requested=false;
 EmergencyRemote::current_data_request=0;EmergencyRemote::resume_release_on_boot=false;
 ack_off=ack_on=send_protection=submit_on_error=false;
 cancellations=on_submits=off_submits=0;
}
void run_until(int64_t end){Sim::stop_at=end;try{EmergencyRemote::worker(nullptr);}catch(const Sim::Stop&){} }
int main(){
 reset();
 Sim::on_delay=[] {
  const auto s=EmergencyRemote::snapshot();
  if(Sim::now<1000001) assert(!s.stop_timed_out && s.busy && !s.quiesced);
  if(s.stop_timed_out) EmergencyRemote::pause_requested=true;
 };
 run_until(1500000);
 auto unknown=EmergencyRemote::snapshot();
 assert(!unknown.output_confirmed && unknown.output_time_us==0 && unknown.stop_timed_out && unknown.quiesced);
 assert(!unknown.busy && !Sim::radio && off_submits==1);
 assert(PowerManager::sleep_block(false,unknown.output_on&&!unknown.stop_timed_out,
        unknown.stop_timed_out,unknown.busy,false)==PowerManager::SleepBlock::None);
 puts("PASS remote: unknown OFF blocked before deadline; bounded timeout stops retry and permits sleep");

 reset();Sim::stop_closed=false;ack_off=true;int phase=0;
 Sim::on_delay=[&]{
  auto s=EmergencyRemote::snapshot();
  if(phase==0 && s.state==EmergencyRemote::State::OFF){EmergencyRemote::request_on();phase=1;}
  else if(phase==1 && s.state==EmergencyRemote::State::STARTING){
   EspNowService::DeviceData old{};EmergencyRemote::receive_data(peer,777,old,true,false,nullptr);
   ack_off=false;EmergencyRemote::request_stop();phase=2;
  }else if(phase==2 && Sim::now<1000000){
   assert(s.output_on && !s.output_confirmed && !s.stop_timed_out);
  }
  if(s.stop_timed_out) EmergencyRemote::pause_requested=true;
 };
 run_until(1500000);auto stale=EmergencyRemote::snapshot();
 assert(phase==2 && stale.output_on && !stale.output_confirmed && stale.stop_timed_out && stale.quiesced);
 puts("PASS remote: old OFF telemetry cannot clear unknown ON; timeout permission preserves uncertainty");

 reset();Sim::stop_closed=false;ack_off=true;send_protection=true;
 Sim::on_delay=[] {if(Sim::now>500000)send_protection=false;};run_until(1800000);
 auto cleared=EmergencyRemote::snapshot();
 assert(cleared.online && cleared.protection_mask==0 && cleared.state==EmergencyRemote::State::OFF);
 puts("PASS remote: cleared protection restores OFF");

 reset();Sim::stop_closed=false;ack_off=ack_on=true;submit_on_error=true;phase=0;
 Sim::on_delay=[&]{if(!phase && EmergencyRemote::model.state==EmergencyRemote::State::OFF){phase=1;EmergencyRemote::request_on();}};
 run_until(400000);assert(on_submits==2 && EmergencyRemote::model.on_attempt==1);
 puts("PASS remote: ON enqueue failure retains intent for bounded retry");

 reset();Sim::stop_closed=false;ack_off=true;phase=0;
 Sim::on_delay=[&]{if(!phase && EmergencyRemote::model.state==EmergencyRemote::State::OFF){phase=1;EmergencyRemote::request_on();}};
 run_until(900000);assert(on_submits==1 && off_submits>=2);
 puts("PASS remote: unconfirmed ON transitions to bounded OFF instead of replaying ON");

 reset();Sim::on_delay=[] {
  if(!EmergencyRemote::model.stop_timed_out){
   EspNowService::DeviceData data{};EmergencyRemote::receive_data(peer,99,data,true,false,nullptr);
  }
 };
 run_until(1500000);
 assert(EmergencyRemote::snapshot().stop_timed_out && !Sim::radio);
 puts("PASS remote: continuous telemetry cannot extend the OFF total deadline");

 reset();Sim::setting_overrides["off_ack_ms"]=2000;bool replied=false;
 Sim::on_delay=[&] {
  if(!replied && Sim::now>=990000){
   EmergencyRemote::receive_switch(peer,request_id,EspNowService::SwitchAction::OFF,
                                  EspNowService::SwitchResult::OK,false,nullptr);replied=true;
  }
 };
 run_until(1100000);
 assert(replied && EmergencyRemote::model.output_confirmed && !EmergencyRemote::model.stop_timed_out);
 puts("PASS remote: queued matching OFF acknowledgement is consumed before terminal timeout");
}
