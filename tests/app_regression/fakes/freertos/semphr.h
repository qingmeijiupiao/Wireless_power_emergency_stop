#pragma once
#include "FreeRTOS.h"
#include "simulation.h"
using SemaphoreHandle_t=Sim::Semaphore*;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return new Sim::Semaphore{true,1};}
inline SemaphoreHandle_t xSemaphoreCreateBinary(){return new Sim::Semaphore{false,0};}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t wait){
 if(s->mutex){s->gate.lock();return pdTRUE;}
 if(!s->count && wait && Sim::on_wait) Sim::on_wait();
 std::unique_lock<std::mutex> guard(s->gate);
 if(!s->count && wait) s->ready.wait_for(guard,std::chrono::milliseconds(1000),[&]{return s->count>0;});
 if(!s->count)return pdFALSE;s->count=0;return pdTRUE;}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s){
 if(s->mutex)s->gate.unlock();else{std::lock_guard<std::mutex> guard(s->gate);s->count=1;s->ready.notify_all();}
 if(s==Sim::hook_sem && Sim::on_give){auto fn=Sim::on_give;fn();}return pdTRUE;}
inline void vSemaphoreDelete(SemaphoreHandle_t s){delete s;}
