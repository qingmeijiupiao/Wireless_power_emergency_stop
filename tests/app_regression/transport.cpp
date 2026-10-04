// Compile production link scheduling/API, and fake only radio/codec primitives.
#include "simulation.h"
#include "espnow_link_task.cpp"
int physical_sends=0;
namespace EspNowLink {
const MacAddress BROADCAST_ADDRESS{{255,255,255,255,255,255}};
bool MacAddress::operator==(const MacAddress&other)const{return memcmp(bytes,other.bytes,6)==0;}
bool MacAddress::operator!=(const MacAddress&other)const{return !(*this==other);}
bool MacAddress::is_broadcast()const{return *this==BROADCAST_ADDRESS;}
}
namespace EspNowLink::Internal {
bool initialized=true,active=true;
QueueHandle_t rx_queue=nullptr,tx_queue=nullptr,mac_queue=nullptr,ack_queue=nullptr;
TaskHandle_t task_handle=nullptr;
HandlerEntry handlers[MAX_HANDLERS]{};PeerEntry peers[MAX_PEERS]{};
PendingTransmission pending{};SendOptions default_reliable_options{};LinkStatistics statistics{};
uint32_t transmission_generation=1,next_sequence=1,local_session_id=1;
portMUX_TYPE statistics_lock=0,state_lock=0;
PeerEntry*find_peer(const MacAddress&address){for(auto&peer:peers)if(peer.used&&peer.config.address==address)return &peer;return nullptr;}
void increment_counter(uint32_t*counter){++*counter;}
bool decode_frame(const uint8_t*,size_t,ParsedFrame*){return false;}
esp_err_t encode_frame(uint8_t,uint16_t,uint32_t,uint32_t,const uint8_t*,size_t,uint8_t*,size_t,size_t*size){*size=20;return ESP_OK;}
}
int main(){
 using namespace EspNowLink;using namespace Internal;
 const MacAddress destination{{1,2,3,4,5,6}};
 peers[0].used=true;peers[0].config.address=destination;peers[0].config.encrypted=true;
 tx_queue=xQueueCreate(TX_QUEUE_LENGTH,sizeof(SendRequest));rx_queue=xQueueCreate(16,sizeof(RxEvent));
 mac_queue=xQueueCreate(8,sizeof(MacResultEvent));ack_queue=xQueueCreate(8,sizeof(AckRequest));
 int cancelled=0;auto callback=[](SendResult result,uint32_t,void*context){if(result==SendResult::CANCELLED)++*static_cast<int*>(context);};
 uint8_t on=1,off=0;
 for(unsigned n=0;n<TX_QUEUE_LENGTH;++n)assert(send(destination,0x201,&on,1,{},callback,&cancelled)==ESP_OK);
 cancel_transmissions_from_isr();
 Sim::stop_at=3000;try{link_task(nullptr);}catch(const Sim::Stop&){}
 assert(cancelled==TX_QUEUE_LENGTH && physical_sends==0);
 puts("PASS transport: ISR cancellation drains full old-ON FIFO without physical sends");

 assert(send(destination,0x201,&on,1,{},callback,&cancelled)==ESP_OK);
 Sim::stop_at=Sim::now+1000;try{link_task(nullptr);}catch(const Sim::Stop&){}
 assert(pending.active && physical_sends==1);
 cancel_transmissions();assert(send(destination,0x201,&off,1)==ESP_OK);
 Sim::stop_at=Sim::now+10000;try{link_task(nullptr);}catch(const Sim::Stop&){}
 assert(!pending.active && physical_sends==1); // Old driver still owns its completion.
 MacResultEvent complete{destination,true};xQueueSend(mac_queue,&complete,0);
 Sim::stop_at=Sim::now+1000;try{link_task(nullptr);}catch(const Sim::Stop&){}
 assert(pending.active && pending.request.payload[0]==0 && physical_sends==2 && statistics.tx_retries==0);
 puts("PASS transport: cancelled in-flight ON never retries; OFF follows driver completion");
 reset_transmit_state();
 assert(!pending.active && driver_owner==DriverOwner::NONE);
 assert(send(destination,0x201,&off,1)==ESP_OK);
 Sim::stop_at=Sim::now+1000;try{link_task(nullptr);}catch(const Sim::Stop&){}
 assert(pending.active && physical_sends==3);
 puts("PASS transport: radio stop discards old driver ownership so user retry can send again");
}
