/*
 * @version: no version
 * @LastEditors: qingmeijiupiao
 * @Description: 黑匣子日志系统实现
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-05-31 19:56:12
 */

#include "blackbox.h"
#include "circular_flash_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace Blackbox;

// 运行状态标志
static bool               enabled              = true;
// FreeRTOS 消息队列句柄，用于存储等待写入 Flash 的日志记录
static QueueHandle_t      log_queue            = nullptr;
// 异步写入任务句柄
static TaskHandle_t       blackbox_task_handle = nullptr;
// 保护启停状态和控制操作，避免清空过程中有新记录进入队列
static SemaphoreHandle_t  state_mutex          = nullptr;
// 队列深度：可缓冲的记录条数。设为 64 可应对短时间的日志爆发
static constexpr uint32_t QUEUE_SIZE           = 64;

enum class QueueItemType : uint8_t {
    WRITE,
    ERASE_ALL,
    BARRIER,
};

struct QueueItem {
    QueueItemType     type;
    Record            record;
    SemaphoreHandle_t completion;
    esp_err_t*        result;
};

/**
 * @brief 计算 CRC8 校验值，用于检测 Flash 数据损坏
 * @param data 数据指针
 * @param length 数据长度
 * @return uint8_t 校验和
 */
static uint8_t CRC8_Calc(const uint8_t* data, size_t length) {
    uint8_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; ++j) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/**
 * @brief 内部物理写入函数。
 * @note 此函数由独立的异步任务调用，包含校验计算和物理 Flash 操作。
 */
static esp_err_t write_record_internal(Record& raw) {
    // 写入前计算 CRC，覆盖除校验位自身外的所有字段
    raw.crc_checksum = CRC8_Calc(reinterpret_cast<uint8_t*>(&raw), sizeof(Record) - 1);
    return CircularFlashBuffer::write_block(reinterpret_cast<uint8_t*>(&raw));
}

/**
 * @brief 黑匣子异步写入任务（消费者）
 * @details 持续监听队列，一旦有新日志则执行 Flash 写入操作。
 */
static void blackbox_task(void* arg) {
    QueueItem item;
    ESP_LOGI("Blackbox", "Async task started");
    while (true) {
        // 无限期等待队列消息
        if (xQueueReceive(log_queue, &item, portMAX_DELAY) == pdPASS) {
            esp_err_t result = ESP_OK;
            switch (item.type) {
            case QueueItemType::WRITE:
                result = write_record_internal(item.record);
                break;
            case QueueItemType::ERASE_ALL:
                result = CircularFlashBuffer::erase_all();
                break;
            case QueueItemType::BARRIER:
                break;
            }
            if (item.result != nullptr) {
                *item.result = result;
            }
            if (item.completion != nullptr) {
                xSemaphoreGive(item.completion);
            }
        }
    }
}

/**
 * @brief 将记录推入异步队列（生产者接口）
 * @param raw 构造好的日志记录
 * @return esp_err_t ESP_OK 表示成功，ESP_FAIL 表示队列满导致丢弃
 */
static esp_err_t queue_item(const QueueItem& item, TickType_t ticks_to_wait = 0) {
    if (log_queue == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // 普通日志使用 0 超时，队列满时立即丢弃，避免保护任务因日志拥堵产生延迟。
    // 控制操作使用 portMAX_DELAY，确保启停和擦除命令可以可靠进入消费者队列。
    if (xQueueSend(log_queue, &item, ticks_to_wait) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t queue_record(const Record& raw, TickType_t ticks_to_wait = 0) {
    QueueItem item = {
        .type       = QueueItemType::WRITE,
        .record     = raw,
        .completion = nullptr,
        .result     = nullptr,
    };
    return queue_item(item, ticks_to_wait);
}

static esp_err_t wait_for_item(QueueItemType type) {
    SemaphoreHandle_t completion = xSemaphoreCreateBinary();
    if (completion == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t operation_result = ESP_OK;
    QueueItem item             = {
                    .type       = type,
                    .record     = {},
                    .completion = completion,
                    .result     = &operation_result,
    };
    esp_err_t err = queue_item(item, portMAX_DELAY);
    if (err == ESP_OK && xSemaphoreTake(completion, portMAX_DELAY) != pdTRUE) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        err = operation_result;
    }
    vSemaphoreDelete(completion);
    return err;
}

static Record make_text_record(const char* text) {
    Record raw           = {};
    raw.header.sof       = CircularFlashBuffer::BLOCK_SOF;
    raw.header.type      = LogType::STRING;
    raw.header.timestamp = esp_timer_get_time() / 1000;
    size_t len           = strnlen(text, PAYLOAD_SIZE - 1);
    memcpy(raw.payload.str, text, len);
    raw.payload.str[len] = '\0';
    return raw;
}

/**
 * @brief 初始化黑匣子系统
 * @details 初始化底层循环缓冲区、创建消息队列并启动异步写入任务。
 */
esp_err_t Blackbox::init() {
    if (log_queue != nullptr) {
        return ESP_OK;
    }

    // 初始化底层驱动（分配分区，设置块大小）
    esp_err_t err = CircularFlashBuffer::init("blackbox", sizeof(Record));
    if (err != ESP_OK)
        return err;

    // 创建 FreeRTOS 队列
    log_queue = xQueueCreate(QUEUE_SIZE, sizeof(QueueItem));
    if (log_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    state_mutex = xSemaphoreCreateMutex();
    if (state_mutex == nullptr) {
        vQueueDelete(log_queue);
        log_queue = nullptr;
        return ESP_ERR_NO_MEM;
    }

    // 创建异步日志任务，优先级设为 3（适中），堆栈 3KB
    BaseType_t ret = xTaskCreate(blackbox_task, "blackbox_t", 3072, nullptr, 3, &blackbox_task_handle);
    if (ret != pdPASS) {
        vSemaphoreDelete(state_mutex);
        state_mutex = nullptr;
        vQueueDelete(log_queue);
        log_queue = nullptr;
        return ESP_ERR_NO_MEM;
    }

    enabled = true;
    return ESP_OK;
}

/**
 * @brief 写入格式化字符串日志
 * @param fmt printf 风格的格式化字符串
 * @details 支持长字符串自动切片存储。
 */
esp_err_t Blackbox::append_text(const char* fmt, ...) {
    if (fmt == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(state_mutex, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    if (!enabled) {
        xSemaphoreGive(state_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    char    buf[TEXT_BUFFER_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    size_t len = strlen(buf);

    // 情况 A：字符串较短，单条 Record 即可容纳
    if (len < PAYLOAD_SIZE) {
        Record raw;
        raw.header.sof       = CircularFlashBuffer::BLOCK_SOF;
        raw.header.type      = LogType::STRING;
        raw.header.timestamp = esp_timer_get_time() / 1000;
        memset(raw.payload.bytes, 0, PAYLOAD_SIZE);
        memcpy(raw.payload.str, buf, len + 1);
        esp_err_t err = queue_record(raw);
        xSemaphoreGive(state_mutex);
        return err;
    }

    // 情况 B：长字符串，需要切分为多个碎片
    uint8_t fragments = (len + PAYLOAD_SIZE) / PAYLOAD_SIZE;
    if (fragments > MAX_TEXT_FRAGMENTS)
        fragments = MAX_TEXT_FRAGMENTS;
    if (uxQueueSpacesAvailable(log_queue) < fragments) {
        xSemaphoreGive(state_mutex);
        return ESP_FAIL;
    }
    uint32_t timestamp = esp_timer_get_time() / 1000;

    for (uint8_t i = 0; i < fragments; i++) {
        Record raw;
        raw.header.sof       = CircularFlashBuffer::BLOCK_SOF;
        raw.header.type      = LogType::STRING;
        raw.header.timestamp = timestamp;
        memset(raw.payload.bytes, 0, PAYLOAD_SIZE);

        size_t offset = i * PAYLOAD_SIZE;
        if (i == fragments - 1) { // 最后一个碎片
            size_t remaining = len - offset;
            if (remaining >= PAYLOAD_SIZE)
                remaining = PAYLOAD_SIZE - 1;
            memcpy(raw.payload.str, buf + offset, remaining);
            raw.payload.str[remaining] = '\0'; // 确保结尾有 NUL
        } else {
            memcpy(raw.payload.str, buf + offset, PAYLOAD_SIZE);
            // 中间碎片不带 NUL，作为后续拼接读取的判断依据
        }

        esp_err_t err = queue_record(raw);
        if (err != ESP_OK) {
            xSemaphoreGive(state_mutex);
            return err;
        }
    }

    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

/**
 * @brief 写入类型化（结构化）原始数据日志
 * @param type 日志类型枚举
 * @param payload 数据指针
 * @param len 数据长度
 */
esp_err_t Blackbox::append_typed(LogType type, const uint8_t* payload, size_t len) {
    if (payload == nullptr || len > PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(state_mutex, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    if (!enabled) {
        xSemaphoreGive(state_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    Record raw;
    raw.header.sof       = CircularFlashBuffer::BLOCK_SOF;
    raw.header.type      = type;
    raw.header.timestamp = esp_timer_get_time() / 1000;
    memset(raw.payload.bytes, 0, PAYLOAD_SIZE);
    memcpy(raw.payload.bytes, payload, len);

    esp_err_t err = queue_record(raw);
    xSemaphoreGive(state_mutex);
    return err;
}

esp_err_t Blackbox::sync() {
    if (log_queue == nullptr || state_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    esp_err_t err = wait_for_item(QueueItemType::BARRIER);
    xSemaphoreGive(state_mutex);
    return err;
}

/**
 * @brief 获取当前存储的总日志条数（不计已被擦除的旧日志）
 */
uint32_t Blackbox::count() {
    return CircularFlashBuffer::get_count();
}

uint32_t Blackbox::capacity() {
    return CircularFlashBuffer::get_capacity();
}

/**
 * @brief 从 Flash 中读取单条原始记录并进行校验
 * @param index 索引（0 为最新记录）
 */
Record Blackbox::read(uint32_t index) {
    Record raw = {};

    esp_err_t err = CircularFlashBuffer::read_block(index, reinterpret_cast<uint8_t*>(&raw));
    if (err != ESP_OK) {
        return raw;
    }

    // 校验 SOF 标志和 CRC8
    uint8_t calc_crc = CRC8_Calc(reinterpret_cast<uint8_t*>(&raw), sizeof(Record) - 1);
    if (raw.header.sof != CircularFlashBuffer::BLOCK_SOF || raw.crc_checksum != calc_crc) {
        ESP_LOGW("BlackBox", "Log at index %d has invalid SOF or CRC8", index);
        raw = {};
    }

    return raw;
}

Blackbox::Snapshot Blackbox::snapshot() { return CircularFlashBuffer::snapshot(); }

esp_err_t Blackbox::read_entry(const Snapshot& view, uint32_t index, Record& record, TextRecord& text) {
    record = {};
    text = {};
    if (index >= view.count) return ESP_ERR_INVALID_ARG;
    Record fragments[MAX_TEXT_FRAGMENTS]{};
    const uint32_t remaining = view.count - index;
    const uint32_t count = remaining < MAX_TEXT_FRAGMENTS ? remaining : MAX_TEXT_FRAGMENTS;
    const auto error = CircularFlashBuffer::read_snapshot(view, index, count, reinterpret_cast<uint8_t*>(fragments));
    if (error != ESP_OK) return error;
    const auto valid = [](const Record& value) {
        return value.header.sof == CircularFlashBuffer::BLOCK_SOF && value.crc_checksum ==
            CRC8_Calc(reinterpret_cast<const uint8_t*>(&value), sizeof(Record) - 1);
    };
    if (!valid(fragments[0])) return ESP_ERR_INVALID_CRC;
    record = fragments[0];
    if (record.header.type != LogType::STRING || !memchr(record.payload.str, 0, PAYLOAD_SIZE)) return ESP_OK;
    text.record_count = 1;
    while (text.record_count < count) {
        const Record& previous = fragments[text.record_count];
        if (!valid(previous) || previous.header.type != LogType::STRING ||
            previous.header.timestamp != record.header.timestamp || memchr(previous.payload.str, 0, PAYLOAD_SIZE)) break;
        ++text.record_count;
    }
    size_t offset = 0;
    for (uint8_t n = text.record_count; n; --n) {
        const size_t length = strnlen(fragments[n - 1].payload.str, PAYLOAD_SIZE);
        memcpy(text.str + offset, fragments[n - 1].payload.str, length);
        offset += length;
    }
    text.str[offset] = 0;
    return ESP_OK;
}

esp_err_t Blackbox::erase_all() {
    if (log_queue == nullptr || state_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(state_mutex, portMAX_DELAY);

    // 丢弃尚未出队的记录。消费者若正在写入，会先完成该次写入，再执行擦除。
    xQueueReset(log_queue);

    esp_err_t err = wait_for_item(QueueItemType::ERASE_ALL);
    if (err == ESP_OK) {
        Record marker = make_text_record("[Blackbox]: reset");
        err           = queue_record(marker, portMAX_DELAY);
        if (err == ESP_OK) {
            err = wait_for_item(QueueItemType::BARRIER);
        }
    }
    xSemaphoreGive(state_mutex);
    return err;
}

/**
 * @brief 读取并拼接完整的文本日志（处理碎片合并）
 * @param index 日志记录索引（应指向长字符串的末尾碎片）
 * @return TextRecord 包含拼接后的字符串和所占用的原始记录条数
 */
TextRecord Blackbox::read_text(uint32_t index) {
    TextRecord text = {};
    Record     fragments[MAX_TEXT_FRAGMENTS];

    // 读取第一条记录（末尾碎片，必然包含 NUL）
    fragments[0] = read(index);
    if (fragments[0].header.sof != CircularFlashBuffer::BLOCK_SOF || fragments[0].header.type != LogType::STRING ||
        memchr(fragments[0].payload.str, '\0', PAYLOAD_SIZE) == nullptr) {
        return text;
    }

    text.record_count  = 1;
    uint32_t raw_count = count();
    // 向前搜索更早的碎片（这些碎片不包含 NUL）
    while (text.record_count < MAX_TEXT_FRAGMENTS && index + text.record_count < raw_count) {
        Record previous = read(index + text.record_count);
        if (previous.header.sof != CircularFlashBuffer::BLOCK_SOF || previous.header.type != LogType::STRING ||
            previous.header.timestamp != fragments[0].header.timestamp ||
            memchr(previous.payload.str, '\0', PAYLOAD_SIZE) != nullptr) {
            break; // 遇到非字符串或带 NUL 的记录，说明碎片结束
        }
        fragments[text.record_count++] = previous;
    }

    size_t offset = 0;
    // 按时间顺序（从旧到新）拼接字符串碎片
    for (uint8_t i = text.record_count; i > 0; --i) {
        const Record& fragment = fragments[i - 1];
        size_t        len      = strnlen(fragment.payload.str, PAYLOAD_SIZE);
        memcpy(text.str + offset, fragment.payload.str, len);
        offset += len;
    }
    text.str[offset] = '\0';

    return text;
}

/**
 * @brief 动态启用或禁用黑匣子功能
 */
void Blackbox::set_enabled(bool enable) {
    if (state_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (enabled == enable) {
        xSemaphoreGive(state_mutex);
        return;
    }

    if (enable) {
        enabled = true;
        queue_record(make_text_record("[Blackbox]: enabled"), portMAX_DELAY);
        wait_for_item(QueueItemType::BARRIER);
        xSemaphoreGive(state_mutex);
        return;
    }
    queue_record(make_text_record("[Blackbox]: disabled"), portMAX_DELAY);
    wait_for_item(QueueItemType::BARRIER);
    enabled = false;
    xSemaphoreGive(state_mutex);
}

/**
 * @brief 查询黑匣子当前是否启用
 */
bool Blackbox::is_enabled() {
    if (state_mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    bool result = enabled;
    xSemaphoreGive(state_mutex);
    return result;
}
