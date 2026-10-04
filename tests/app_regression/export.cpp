// Compile production ring and Blackbox snapshot reads over a simulated Flash partition.
#include "simulation.h"
#include "../../components/overrides/circular_flash_buffer/src/circular_flash_buffer.cpp"
#include "../../components/overrides/blackbox/blackbox.cpp"
void append(const char*text,uint32_t time){
 Blackbox::Record record{};record.header.sof=CircularFlashBuffer::BLOCK_SOF;
 record.header.type=Blackbox::LogType::STRING;record.header.timestamp=time;
 strncpy(record.payload.str,text,Blackbox::PAYLOAD_SIZE-1);
 assert(write_record_internal(record)==ESP_OK);
}
int main(){
 memset(flash_memory,255,sizeof(flash_memory));assert(Blackbox::init()==ESP_OK);
 append("first",1);append("second",2);const auto view=Blackbox::snapshot();
 append("third",3);append("fourth",4);
 Blackbox::Record record{};Blackbox::TextRecord text{};
 assert(Blackbox::read_entry(view,0,record,text)==ESP_OK && record.header.timestamp==2 && std::string(text.str)=="second");
 assert(Blackbox::read_entry(view,1,record,text)==ESP_OK && record.header.timestamp==1 && std::string(text.str)=="first");
 puts("PASS export: concurrent appends cannot shift snapshot header/text or duplicate entries");

 for(unsigned n=0;n<400;++n)append("overwrite",100+n);
 assert(Blackbox::read_entry(view,0,record,text)==ESP_ERR_INVALID_STATE);
 auto after_wrap=Blackbox::snapshot();assert(after_wrap.count<=Blackbox::capacity());
 assert(Blackbox::read_entry(after_wrap,0,record,text)==ESP_OK && record.header.timestamp==499);
 assert(CircularFlashBuffer::erase_all()==ESP_OK);
 assert(Blackbox::read_entry(after_wrap,0,record,text)==ESP_ERR_INVALID_STATE);
 puts("PASS export: wrap and erase invalidate stale snapshots instead of returning replacement records");

 Blackbox::Record prefix{};prefix.header.sof=CircularFlashBuffer::BLOCK_SOF;
 prefix.header.type=Blackbox::LogType::STRING;prefix.header.timestamp=1000;
 memset(prefix.payload.str,'A',Blackbox::PAYLOAD_SIZE);assert(write_record_internal(prefix)==ESP_OK);
 append("tail",1000);auto fragmented=Blackbox::snapshot();append("later",1001);
 assert(Blackbox::read_entry(fragmented,0,record,text)==ESP_OK && text.record_count==2 && record.header.timestamp==1000);
 assert(std::string(text.str)==std::string(Blackbox::PAYLOAD_SIZE,'A')+"tail");
 puts("PASS export: fragmented text and timestamp are read in one storage transaction");
}
