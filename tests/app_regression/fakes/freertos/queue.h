#pragma once
#include "task.h"
#include "simulation.h"
using QueueHandle_t=Sim::Queue*;
inline QueueHandle_t xQueueCreate(unsigned capacity,size_t size){return new Sim::Queue{capacity,size,{}};}
inline BaseType_t xQueueSend(QueueHandle_t q,const void *p,TickType_t){
 if(q->items.size()==q->capacity)return pdFALSE;
 const auto *b=static_cast<const unsigned char*>(p);q->items.emplace_back(b,b+q->size);return pdTRUE;}
inline BaseType_t xQueueReceive(QueueHandle_t q,void *p,TickType_t ticks){
 if(q->items.empty()){if(ticks) vTaskDelay(ticks==portMAX_DELAY?1:ticks);return pdFALSE;}std::memcpy(p,q->items.front().data(),q->size);q->items.pop_front();return pdTRUE;}

inline BaseType_t xQueueOverwrite(QueueHandle_t q,const void*p){q->items.clear();return xQueueSend(q,p,0);}
inline void xQueueReset(QueueHandle_t q){q->items.clear();}
inline void vQueueDelete(QueueHandle_t q){delete q;}
inline unsigned uxQueueSpacesAvailable(QueueHandle_t q){return q->capacity-q->items.size();}
