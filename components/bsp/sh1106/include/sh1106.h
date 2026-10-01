/**
 * @file sh1106.h
 * @brief SH1106 OLED 的 I2C 驱动接口：初始化、整帧写入与关闭。
 */
#pragma once
#include "driver/i2c_master.h"
#include <stddef.h>
#include <stdint.h>

// SH1106 的 I2C 从机地址，常见为 0x3c 或 0x3d，由 init 时探测确定。
// 单任务调用；帧缓冲格式为 8 页，每页 128 列，低位像素在上。
class Sh1106 {
public:
    // 初始化 I2C 总线与屏幕：探测地址、下发初始化命令并清屏。
    // offset 为 SH1106 相对 SSD1306 的列偏移（以像素计）。
    esp_err_t init(int sda, int scl, uint8_t offset = 2);
    // 写入一整帧图像；frame 大小必须为 1024 字节（8 页 × 128 列）。
    esp_err_t write_frame(const uint8_t* frame, size_t size);
    // 关闭显示并释放 I2C 设备与总线资源。
    void shutdown();
    // 返回探测到的 I2C 从机地址。
    uint8_t address() const { return address_; }
    // 析构时自动调用 shutdown，确保资源被释放。
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
