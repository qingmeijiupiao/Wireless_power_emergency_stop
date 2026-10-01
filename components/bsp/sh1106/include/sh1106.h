#pragma once
#include "driver/i2c_master.h"
#include <stddef.h>
#include <stdint.h>

// 单任务调用；帧缓冲格式为 8 页，每页 128 列，低位像素在上。
class Sh1106 {
public:
    esp_err_t init(int sda, int scl, uint8_t offset = 2);
    esp_err_t write_frame(const uint8_t* frame, size_t size);
    void shutdown();
    uint8_t address() const { return address_; }
    ~Sh1106();
    Sh1106() = default;
    Sh1106(const Sh1106&) = delete;
    Sh1106& operator=(const Sh1106&) = delete;
private:
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
    uint8_t address_ = 0;
    uint8_t offset_ = 2;
};
