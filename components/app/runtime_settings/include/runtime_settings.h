#pragma once
#include <cstdint>

/**
 * @file runtime_settings.h
 * @brief 急停控制器运行参数的类型化访问与持久化接口
 */
namespace RuntimeSettings {

// 自动休眠时间菜单的候选值(ms)：5、10、30、60 分钟，下标供 UI 选择使用。
constexpr uint32_t kSleepTimesMs[] = {300000, 600000, 1800000, 3600000};

/** 运行参数索引，与私有参数表逐项对应；字符串名称仅用于 Shell/持久化。 */
enum class Id : uint8_t { ConnectMs, IdleMs, MenuIdleMs, NoticeMs, ReleaseMs, OffAckMs,
    OffRetryMs, OnAckMs, FreshMs, BatteryMs, ReportMs, LowMv, Count };
/** @brief 按类型无锁读取参数；非法索引返回 0。 */
uint32_t get(Id id);
/** @brief 按类型设置参数，范围校验、持久化/发布和事件入队完整串行。
 * @param source 操作来源，如 ui/shell/app；在调用内复制到日志文本。
 */
bool set(Id id, uint32_t value, const char* source = "app");

// 从 NVS 载入全部参数到原子缓存，必须在其它模块读取前调用一次。
void init();

// 查询/切换“常亮(禁止自动休眠)”开关；source 同 set()，结果和旧/新值均记录。
bool always_on();
bool set_always_on(bool enabled, const char* source = "app");

// 按名称读取/写入整型参数；get 对未知名称返回 0，set 会做范围校验。
uint32_t get(const char *name);
bool set(const char *name, uint32_t value, const char* source = "app");

// 将当前全部参数及其有效范围打印到标准输出(命令层使用)。
void print();

// 把当前全部参数写入黑匣子，作为一次配置快照。
void record_snapshot();

} // namespace RuntimeSettings
