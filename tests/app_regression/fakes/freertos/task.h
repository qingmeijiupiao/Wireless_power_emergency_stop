#pragma once
#include "FreeRTOS.h"
#include "simulation.h"
inline TickType_t xTaskGetTickCount(){return Sim::now/1000;}
inline void vTaskDelay(TickType_t ticks){Sim::now+=ticks*1000LL;if(Sim::on_delay) Sim::on_delay();
 if(Sim::stop_at && Sim::now>=Sim::stop_at) throw Sim::Stop{};}
inline void vTaskDelete(void*){}
inline BaseType_t xTaskCreate(void(*fn)(void*),const char*name,unsigned,void *arg,int,TaskHandle_t*){
 Sim::tasks[name]=fn;return pdPASS;}
