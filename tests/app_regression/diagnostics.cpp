// Production queue boundedness and flush lifetime, without synchronous console output in submit.
#include "../../components/app/app_diagnostics/src/app_diagnostics.cpp"
int main(){
 using namespace AppDiagnostics;
 assert(init()==ESP_OK);
 for(int n=0;n<40;++n)write(ESP_LOG_INFO,"ProductEvent","event %d",n);
 assert(dropped()==8 && Sim::logs.empty());
 assert(!flush(0));
 Sim::stop_at=1000;
 try{worker(nullptr);}catch(const Sim::Stop&){}
 assert(Sim::logs.size()==32 && flush(0));
 puts("PASS diagnostics: zero-wait fixed queue, visible overflow and safe flush timeout");
}
