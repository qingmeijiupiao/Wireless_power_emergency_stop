#pragma once
#include <cstdint>
namespace RuntimeSettings {
void init();
uint32_t get(const char* name);
bool set(const char* name, uint32_t value);
void print();
}
