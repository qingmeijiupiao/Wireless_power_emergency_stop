/*
 * @LastEditors: qingmeijiupiao
 * @Description: 基于ESP32 SPI Flash分区的循环缓冲区驱动，与具体数据结构无关
 * @Author: qingmeijiupiao
 */
#ifndef CIRCULAR_FLASH_BUFFER_H
#define CIRCULAR_FLASH_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

namespace CircularFlashBuffer {

constexpr uint32_t PAGE_SIZE        = 256;
constexpr uint32_t SECTOR_SIZE      = 4096;
constexpr uint32_t PAGES_PER_SECTOR = SECTOR_SIZE / PAGE_SIZE;

constexpr uint8_t BLOCK_SOF = 0xAA;

/**
 * @brief 初始化指定分区上的循环 Flash 缓冲区。
 * @param partition_name 分区名称。
 * @param block_size
 * 固定记录块大小，必须整除 Flash 页大小。
 * @return ESP_OK 初始化成功；其他值表示参数、分区或同步资源错误。

 */
esp_err_t init(const char* partition_name, size_t block_size);

/**
 * @brief 追加一个固定大小的数据块。
 * @param data 待写入的数据块。
 * @return ESP_OK
 * 写入成功；其他值表示未初始化、已禁用或 Flash 写入失败。
 */
esp_err_t write_block(const uint8_t* data);

/**
 * @brief 按新到旧顺序读取一个数据块。
 * @param index 记录索引，0 表示最新记录。
 * @param data
 * 接收数据块的缓冲区。
 * @return ESP_OK 读取成功；其他值表示参数、范围或 Flash 读取错误。

 */
esp_err_t read_block(uint32_t index, uint8_t* data);

/** @brief 固定读取代际；重启、擦除或记录被覆盖后读取明确失败。 */
struct Snapshot {
    uint64_t written = 0;
    uint32_t epoch = 0;
    uint32_t count = 0;
};
/** @brief 短暂持锁获取固定快照，不暂停写入者。 */
Snapshot snapshot();
/** @brief 一次锁内读取快照的连续记录；data 至少容纳 count * block_size 字节。
 * @return ESP_ERR_INVALID_STATE 记录已被覆盖或快照代际失效；其他错误透传。
 */
esp_err_t read_snapshot(const Snapshot& snapshot, uint32_t index, uint32_t count, uint8_t* data);

/**
 * @brief 物理擦除整个分区并重置内部指针
 */
esp_err_t erase_all();

/** @return 当前有效数据块数量。 */
uint32_t get_count();

/** @brief 返回环形缓冲最多可保留的数据块数量。 */
uint32_t get_capacity();

/**
 * @brief 设置循环缓冲区写入开关。
 * @param enable true 允许写入；false 拒绝后续写入。
 */
void set_enable(bool enable);
} // namespace CircularFlashBuffer

#endif
