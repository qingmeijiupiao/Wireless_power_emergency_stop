/*
 * @LastEditors: qingmeijiupiao
 * @Description: 基于ESP32 SPI Flash分区的循环缓冲区驱动实现
 * @Author: qingmeijiupiao
 */
#include "circular_flash_buffer.h"
#include <stddef.h>
#include "esp_partition.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstring>

using namespace CircularFlashBuffer;

static esp_partition_t* cfb_partition       = nullptr;
static size_t           cfb_block_size      = 0;
static uint8_t          cfb_blocks_per_page = 0;

static bool              cfb_enable = true;
static SemaphoreHandle_t cfb_mutex  = nullptr;

static uint32_t now_write_page      = 0;
static uint8_t  block_page_index    = 0;
static uint64_t written_blocks = 0;
static uint32_t storage_epoch = 1;
static uint32_t block_count         = 0;
static uint32_t max_retained_blocks = 0;
static uint32_t total_pages         = 0;
static uint32_t total_sectors       = 0;

static uint8_t page_buffer[PAGE_SIZE];

static esp_err_t erase_sector(uint32_t sector_index) {
    if (sector_index >= total_sectors) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_partition_erase_range(cfb_partition, sector_index * SECTOR_SIZE, SECTOR_SIZE);
}

static uint32_t count_valid_blocks_in_sector(uint32_t sector_index) {
    uint8_t        buffer[PAGE_SIZE];
    uint32_t       count      = 0;
    const uint32_t first_page = sector_index * PAGES_PER_SECTOR;

    for (uint32_t page = 0; page < PAGES_PER_SECTOR; ++page) {
        if (esp_partition_read(cfb_partition, (first_page + page) * PAGE_SIZE, buffer, PAGE_SIZE) != ESP_OK) {
            return 0;
        }
        for (uint8_t block = 0; block < cfb_blocks_per_page; ++block) {
            if (buffer[block * cfb_block_size] == BLOCK_SOF) {
                ++count;
            }
        }
    }
    return count;
}

static esp_err_t write_page(uint32_t page_index, const uint8_t* data) {
    if (page_index >= total_pages) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_partition_write(cfb_partition, page_index * PAGE_SIZE, data, PAGE_SIZE);
}

static uint8_t get_empty_block_index_from_page(const uint8_t* page_data) {
    for (size_t i = 0; i < cfb_blocks_per_page; i++) {
        if (page_data[i * cfb_block_size] != BLOCK_SOF) {
            return i;
        }
    }
    return cfb_blocks_per_page;
}

esp_err_t CircularFlashBuffer::init(const char* partition_name, size_t block_size) {
    // 1. 参数合法性校验
    if (block_size == 0 || PAGE_SIZE % block_size != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cfb_block_size      = block_size;
    cfb_blocks_per_page = PAGE_SIZE / block_size;
    alignas(4) uint8_t buffer[PAGE_SIZE];

    // 2. 获取分区句柄
    const esp_partition_t* _partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, partition_name);
    if (_partition == nullptr) {
        ESP_LOGE("CircularFlashBuffer", "Partition not found: %s", partition_name);
        return ESP_ERR_NOT_FOUND;
    }

    cfb_partition = (esp_partition_t*)_partition;
    total_pages   = cfb_partition->size / PAGE_SIZE;
    total_sectors = cfb_partition->size / SECTOR_SIZE;

    if (total_sectors < 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    // 最大块数 = (总扇区 - 1预留) * 每个扇区的块数
    max_retained_blocks = (total_sectors - 1) * SECTOR_SIZE / cfb_block_size;
    memset(page_buffer, 0xFF, PAGE_SIZE);

    // 3. 寻找第一个物理空页 (0xFF...)
    // 目的：快速定位数据区与空区的分界线
    bool empty_page_found = false;
    for (uint32_t page = 0; page < total_pages; ++page) {
        esp_partition_read(cfb_partition, page * PAGE_SIZE, buffer, PAGE_SIZE);
        if (!memcmp(buffer, page_buffer, PAGE_SIZE)) {
            now_write_page   = page;
            empty_page_found = true;
            break;
        }
    }

    if (!empty_page_found) {
        ESP_LOGE("CircularFlashBuffer", "No empty page found! Flash error or partition full without circularity.");
        return ESP_ERR_INVALID_STATE;
    }

    // 4. 精确判定写入头 (Logical Head) 的位置
    if (now_write_page == 0) {
        // 情况 A：物理起始位置就是空的。
        // 关键临界点：当 Head 处于最后一个扇区时，0 号扇区会被预擦除。
        // 我们需要通过检查分区末尾的“连续性”来区分是“全新”还是“正在跨越物理边界”。
        const uint32_t last_sector_start     = (total_sectors - 1) * PAGES_PER_SECTOR;
        const uint32_t previous_sector_start = (total_sectors - 2) * PAGES_PER_SECTOR;

        esp_partition_read(cfb_partition, last_sector_start * PAGE_SIZE, buffer, PAGE_SIZE);
        bool last_sector_started = memcmp(buffer, page_buffer, PAGE_SIZE) != 0;
        esp_partition_read(cfb_partition, previous_sector_start * PAGE_SIZE, buffer, PAGE_SIZE);
        bool previous_sector_started = memcmp(buffer, page_buffer, PAGE_SIZE) != 0;

        if (!previous_sector_started) {
            // 全新状态：倒数第二个扇区仍为空，不可能已经循环写到末尾。
            now_write_page   = 0;
            block_page_index = 0;
        } else if (!last_sector_started) {
            // 刚进入最后一个扇区，0 号扇区已经预擦除，但末扇区还没有写入。
            now_write_page   = last_sector_start;
            block_page_index = 0;
        } else {
            // 最后一个扇区已经开始写入。优先在扇区内定位可写页；若整个扇区
            // 均已写满，则 Head 已跨越物理边界回到 0 号页。
            bool writable_page_found = false;
            for (uint32_t p = last_sector_start; p < total_pages; p++) {
                esp_partition_read(cfb_partition, p * PAGE_SIZE, buffer, PAGE_SIZE);
                uint8_t index = get_empty_block_index_from_page(buffer);
                if (index != cfb_blocks_per_page) {
                    now_write_page      = p;
                    block_page_index    = index;
                    writable_page_found = true;
                    break;
                }
            }
            if (!writable_page_found) {
                now_write_page   = 0;
                block_page_index = 0;
            }
        }
    } else {
        // 情况 B：物理开头有数据，中间发现空页。Head 在前一个已写页内。
        esp_partition_read(cfb_partition, (now_write_page - 1) * PAGE_SIZE, buffer, PAGE_SIZE);
        uint8_t index = get_empty_block_index_from_page(buffer);
        if (index != cfb_blocks_per_page) {
            // 前一页没写满，Head 就在那
            now_write_page   = now_write_page - 1;
            block_page_index = index;
        } else {
            // 前一页刚好写满，Head 在当前空页开头
            block_page_index = 0;
        }
    }

    // 5. 边界一致性自检 (Consistency Check)
    // 识别并纠正由于 erase_all 线性擦除中途掉电导致的“幽灵数据”
    constexpr uint32_t SENTINEL_SECTOR = 2;
    if (now_write_page >= total_pages - 1 && total_sectors > SENTINEL_SECTOR) {
        if (count_valid_blocks_in_sector(SENTINEL_SECTOR) == 0) {
            ESP_LOGW("CircularFlashBuffer", "Consistency error detected. Performing recovery erase...");
            esp_partition_erase_range(cfb_partition, 0, cfb_partition->size);
            now_write_page   = 0;
            block_page_index = 0;
            memset(page_buffer, 0xFF, PAGE_SIZE);
        }
    }

    // 6. 计算有效块总数
    uint32_t blocks_per_sector = PAGES_PER_SECTOR * cfb_blocks_per_page;
    uint32_t current_sector    = now_write_page / PAGES_PER_SECTOR;
    bool     is_wrapped        = current_sector >= total_sectors - 2;

    if (!is_wrapped && now_write_page < total_pages - PAGES_PER_SECTOR) {
        // Head 已越过物理边界后，末扇区仍保留旧数据；只检查其第一页即可
        // 区分正常线性增长和回绕后的低地址 Head。
        uint32_t last_sector_start = (total_sectors - 1) * PAGES_PER_SECTOR;
        esp_partition_read(cfb_partition, last_sector_start * PAGE_SIZE, buffer, PAGE_SIZE);
        is_wrapped = buffer[0] == BLOCK_SOF;
    }

    if (!is_wrapped) {
        // 线性增长阶段：数据从 0 开始
        block_count = now_write_page * cfb_blocks_per_page + block_page_index;
    } else {
        // 回绕阶段：有效数据 = (总扇区 - 2个预留/间隙扇区) * 扇区容量 + 当前扇区已写量
        uint32_t current_sector_offset = (now_write_page % PAGES_PER_SECTOR) * cfb_blocks_per_page + block_page_index;
        block_count                    = (total_sectors - 2) * blocks_per_sector + current_sector_offset;
    }

    if (block_count > max_retained_blocks) {
        block_count = max_retained_blocks;
    }

    // 7. 载入当前页缓存并初始化锁
    esp_partition_read(cfb_partition, now_write_page * PAGE_SIZE, buffer, PAGE_SIZE);
    memcpy(page_buffer, buffer, PAGE_SIZE);

    cfb_mutex = xSemaphoreCreateBinary();
    if (cfb_mutex == nullptr)
        return ESP_ERR_NO_MEM;
    xSemaphoreGive(cfb_mutex);

    return ESP_OK;
}

esp_err_t CircularFlashBuffer::write_block(const uint8_t* data) {
    if (data == nullptr || cfb_mutex == nullptr)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(cfb_mutex, portMAX_DELAY);
    if (!cfb_enable) {
        xSemaphoreGive(cfb_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // 1. 数据暂存至内存页缓冲区
    size_t  offset = block_page_index * cfb_block_size;
    uint8_t previous_block[PAGE_SIZE];
    memcpy(previous_block, page_buffer + offset, cfb_block_size);
    memcpy(page_buffer + offset, data, cfb_block_size);

    // 2. 将更新后的页写回 Flash (由于 Flash 位能从 1 变 0，所以页内连续写 Block 无需先擦除)
    if (write_page(now_write_page, page_buffer) != ESP_OK) {
        ESP_LOGE("CircularFlashBuffer", "Flash write failed at page %lu", now_write_page);
        memcpy(page_buffer + offset, previous_block, cfb_block_size);
        xSemaphoreGive(cfb_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ++written_blocks; // 即使随后的预擦除失败，也已经追加了一条记录。
    // 3. 物理写入成功后再更新计数，避免失败记录被暴露给读取方
    if (block_count < max_retained_blocks) {
        block_count++;
    }

    // 4. 更新 Head 索引
    block_page_index = (block_page_index + 1) % cfb_blocks_per_page;

    if (block_page_index == 0) {
        // 当前页写满了，切换到下一页
        now_write_page = (now_write_page + 1) % total_pages;
        memset(page_buffer, 0xFF, PAGE_SIZE);

        // 每当写完一个完整扇区 (4KB) 时，执行维护操作
        if (now_write_page % PAGES_PER_SECTOR == 0) {
            uint32_t sector_index       = now_write_page / PAGES_PER_SECTOR;
            // 预擦除“下下个”扇区，确保存储环路始终有一个“空白间隙”
            uint32_t erase_sector_index = (sector_index + 1) % total_sectors;

            if (erase_sector(erase_sector_index) != ESP_OK) {
                ESP_LOGE("CircularFlashBuffer", "Pre-erase failed at sector %lu", erase_sector_index);
                xSemaphoreGive(cfb_mutex);
                return ESP_ERR_INVALID_STATE;
            }

            // 擦除成功后再扣减旧数据，避免错误路径提前隐藏仍可读取的记录。
            uint32_t blocks_per_sector = PAGES_PER_SECTOR * cfb_blocks_per_page;
            if (block_count > (max_retained_blocks - blocks_per_sector)) {
                block_count -= blocks_per_sector;
            }
        }
    }

    xSemaphoreGive(cfb_mutex);
    return ESP_OK;
}

esp_err_t CircularFlashBuffer::read_block(uint32_t index, uint8_t* data) {
    if (data == nullptr || cfb_mutex == nullptr)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(cfb_mutex, portMAX_DELAY);

    // 1. 范围校验：index = 0 表示最新的一条记录
    if (index >= block_count) {
        xSemaphoreGive(cfb_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    // 2. 定位“最新记录”所在的物理位置
    uint32_t last_page;
    uint8_t  last_offset;

    if (block_page_index == 0) {
        // 如果当前 Head 在页开头，那最新记录就在上一页的末尾
        last_page   = (now_write_page == 0) ? (total_pages - 1) : (now_write_page - 1);
        last_offset = cfb_blocks_per_page - 1;
    } else {
        // 否则就在当前页的前一个位置
        last_page   = now_write_page;
        last_offset = block_page_index - 1;
    }

    // 3. 根据 index 向后追溯物理地址
    uint32_t target_page   = last_page;
    uint8_t  target_offset = last_offset;
    uint32_t remaining     = index;

    while (remaining > 0) {
        if (target_offset >= remaining) {
            // 偏移量在当前页内就够了
            target_offset -= remaining;
            remaining      = 0;
        } else {
            // 需要跨页回溯
            remaining     -= (target_offset + 1);
            target_page    = (target_page == 0) ? (total_pages - 1) : (target_page - 1);
            target_offset  = cfb_blocks_per_page - 1;
        }
    }

    // 4. 从 Flash 读取目标页并提取数据
    uint8_t   buffer[PAGE_SIZE];
    esp_err_t err = esp_partition_read(cfb_partition, target_page * PAGE_SIZE, buffer, PAGE_SIZE);
    if (err == ESP_OK) {
        memcpy(data, buffer + target_offset * cfb_block_size, cfb_block_size);
    }

    xSemaphoreGive(cfb_mutex);
    return err;
}

esp_err_t CircularFlashBuffer::erase_all() {
    if (cfb_partition == nullptr || cfb_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(cfb_mutex, portMAX_DELAY);

    ++storage_epoch; // 部分擦除失败也必须使旧快照失效。
    // 1. 物理擦除整个分区
    esp_err_t err = esp_partition_erase_range(cfb_partition, 0, cfb_partition->size);
    if (err != ESP_OK) {
        ESP_LOGE("CircularFlashBuffer", "Failed to erase partition");
        xSemaphoreGive(cfb_mutex);
        return err;
    }

    // 2. 重置所有内部管理变量
    now_write_page   = 0;
    block_page_index = 0;
    block_count      = 0;
    memset(page_buffer, 0xFF, PAGE_SIZE);

    ESP_LOGI("CircularFlashBuffer", "Partition erased and pointers reset");

    xSemaphoreGive(cfb_mutex);
    return ESP_OK;
}

CircularFlashBuffer::Snapshot CircularFlashBuffer::snapshot() {
    if (!cfb_mutex) return {};
    xSemaphoreTake(cfb_mutex, portMAX_DELAY);
    const Snapshot result{written_blocks, storage_epoch, block_count};
    xSemaphoreGive(cfb_mutex);
    return result;
}

esp_err_t CircularFlashBuffer::read_snapshot(const Snapshot& view, uint32_t index, uint32_t count, uint8_t* data) {
    if (!cfb_mutex || !data) return ESP_ERR_INVALID_STATE;
    if (!count || index >= view.count || count > view.count - index) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(cfb_mutex, portMAX_DELAY);
    const uint64_t delta = written_blocks - view.written;
    if (view.epoch != storage_epoch || view.written > written_blocks ||
        delta + index + count > block_count) {
        xSemaphoreGive(cfb_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const uint64_t physical_capacity = uint64_t(total_pages) * cfb_blocks_per_page;
    const uint64_t head = uint64_t(now_write_page) * cfb_blocks_per_page + block_page_index;
    esp_err_t error = ESP_OK;
    for (uint32_t n = 0; n < count; ++n) {
        const uint64_t offset = (head + physical_capacity - 1 - (delta + index + n)) % physical_capacity;
        error = esp_partition_read(cfb_partition, offset * cfb_block_size, data + n * cfb_block_size, cfb_block_size);
        if (error != ESP_OK) break;
    }
    xSemaphoreGive(cfb_mutex);
    return error;
}

uint32_t CircularFlashBuffer::get_count() {
    if (cfb_mutex == nullptr) {
        return 0;
    }
    xSemaphoreTake(cfb_mutex, portMAX_DELAY);
    uint32_t count = block_count;
    xSemaphoreGive(cfb_mutex);
    return count;
}

uint32_t CircularFlashBuffer::get_capacity() {
    if (cfb_mutex == nullptr) {
        return 0;
    }
    xSemaphoreTake(cfb_mutex, portMAX_DELAY);
    uint32_t capacity = max_retained_blocks;
    xSemaphoreGive(cfb_mutex);
    return capacity;
}

void CircularFlashBuffer::set_enable(bool enable) {
    if (cfb_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(cfb_mutex, portMAX_DELAY);
    cfb_enable = enable;
    xSemaphoreGive(cfb_mutex);
}
