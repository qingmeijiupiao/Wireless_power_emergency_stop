// Production queue boundedness and flush lifetime, without synchronous console output in submit.
#include "../../components/app/app_diagnostics/src/app_diagnostics.cpp"
int main(){
 using namespace AppDiagnostics;
 assert(init()==ESP_OK);
 for(int n=0;n<40;++n)write(ESP_LOG_INFO,"AppRuntime","status %d",n);
 for(int n=0;n<33;++n)write(ESP_LOG_INFO,"ProductEvent","STOP event %d",n);
 assert(dropped()==25 && event_losses==1 && Sim::logs.empty());
 assert(!flush(0));
 Sim::stop_at=1000;
 try{worker(nullptr);}catch(const Sim::Stop&){}
 assert(Sim::logs.size()==49 && flush(0));
 assert(Sim::logs[0].find("e=1 t=0 STOP event 0")!=std::string::npos);
 assert(Sim::logs.back().find("log gap dropped=25 events=1")!=std::string::npos);
 puts("PASS diagnostics: routine flood preserves event capacity; losses and producer time remain visible");
 Sim::stop_at=0;Sim::logs.clear();Sim::log_entries.clear();
 Sim::now=123000;write(ESP_LOG_INFO,"ProductEvent","%s",std::string(200,'x').c_str());
 Sim::now=999000;Sim::stop_at=1000000;
 try{worker(nullptr);}catch(const Sim::Stop&){}
 assert(Sim::logs[0].find("t=123 ")!=std::string::npos && Sim::logs[0].find("[cut]")!=std::string::npos);
 assert(Sim::logs[0].size()<180 && Sim::logs.back().find("truncated=1")!=std::string::npos);
 puts("PASS diagnostics: persistence envelope keeps occurrence time and explicitly marks truncated content");
}
