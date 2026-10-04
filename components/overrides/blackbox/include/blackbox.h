/*
 * @LastEditors: qingmeijiupiao
 * @Description: 黑匣子日志系统，支持字符串日志与类型化数据日志，与具体业务数据结构解耦
 * @Author: qingmeijiupiao
 */
#ifndef BLACKBOX_H
#define BLACKBOX_H
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "circular_flash_buffer.h"

namespace Blackbox {
constexpr uint32_t RECORD_SIZE = 32;

enum class LogType : uint8_t {
    STRING     = 0,
    STRUCTURED = 1,
};

struct RecordHeader {
    uint8_t  sof;
    LogType  type;
    uint32_t timestamp;
} __attribute__((packed));

constexpr uint8_t PAYLOAD_SIZE       = RECORD_SIZE - sizeof(RecordHeader) - 1;
/** 单条字符串日志最多占用的原始记录数。最大字符数 = PAYLOAD_SIZE*MAX_TEXT_FRAGMENTS - 1 */
constexpr uint8_t MAX_TEXT_FRAGMENTS = 8;
/** 拼接字符串日志所需的缓冲区大小，包含结尾的 NUL 字符。 */
constexpr size_t  TEXT_BUFFER_SIZE   = MAX_TEXT_FRAGMENTS * PAYLOAD_SIZE;

union RecordPayload {
    uint8_t bytes[PAYLOAD_SIZE];
    char    str[PAYLOAD_SIZE];
} __attribute__((packed));

struct Record {
    RecordHeader  header;
    RecordPayload payload;
    uint8_t       crc_checksum;
} __attribute__((packed));
static_assert(sizeof(Record) == RECORD_SIZE, "Blackbox record size mismatch");

/** 由一条或多条原始记录拼接得到的完整字符串日志。 */
struct TextRecord {
    /** 以 NUL 结尾的完整字符串。 */
    char    str[TEXT_BUFFER_SIZE];
    /** 本次读取占用的原始记录数，为零表示未读取到字符串。 */
    uint8_t record_count;
};

/** @brief 初始化黑匣子分区、队列和异步写入任务。 */
esp_err_t init();

/** @brief 格式化并异步追加文本日志。 */
esp_err_t append_text(const char* fmt, ...);

/** @brief 异步追加一条类型化二进制记录。 */
esp_err_t append_typed(LogType type, const uint8_t* payload, size_t len);

/**
 * @brief 等待此前已入队的日志处理完成
 */
esp_err_t sync();

/** @return 当前底层有效原始记录数。 */
uint32_t count();

/** @brief 返回最多可保留的原始记录数。 */
uint32_t capacity();

/** @brief 按新到旧顺序读取一条原始记录。 */
Record read(uint32_t index);

using Snapshot = CircularFlashBuffer::Snapshot;
/** @brief 获取固定导出快照（调用前由产品层完成日志 flush/sync）。 */
Snapshot snapshot();
/** @brief 从同一快照一次性读取 header 和完整文本，不暂停日志生产。
 * @return ESP_OK 有效记录；ESP_ERR_INVALID_STATE 已覆盖/擦除；其他错误透传。
 */
esp_err_t read_entry(const Snapshot& snapshot, uint32_t index, Record& record, TextRecord& text);

/**
 * @brief 清空所有日志并重置黑匣子状态
 */
esp_err_t erase_all();

/**
 * @brief 读取并拼接字符串日志。
 *
 * index 必须指向字符串日志最新的一条原始记录。原始记录按从新到旧的
 * 顺序读取，因此成功读取后，调用方应将轮询索引增加
 * TextRecord::record_count。
 *
 * @param index 按从新到旧顺序计算的原始记录索引。
 * @return 完整字符串和占用的原始记录数。记录无效、记录不是字符串类型，
 *         或索引指向非末尾字符串分片时，record_count 为零。
 */
TextRecord read_text(uint32_t index);

/** @brief 设置黑匣子记录开关。 */
void set_enabled(bool enable);

/** @return true 表示黑匣子当前允许追加记录。 */
bool is_enabled();
} // namespace Blackbox

#endif
