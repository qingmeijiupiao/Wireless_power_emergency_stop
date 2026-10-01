/**
 * @file runtime_settings.cpp
 * @brief 运行参数表的持久化、范围校验与原子读取实现
 *
 * 设计约束：
 * - 参数通过公共 HXC_NVS 持久化，名称、默认值与范围集中维护在本文件的表里；
 * - 运行期读取走原子缓存，避免工作线程与 Shell 任务之间的数据竞争。
 */
#include "runtime_settings.h"
#include "HXC_NVS.h"
#include "diagnostic_log.h"
#include "esp_log.h"
#include <atomic>
#include <cstdio>
#include <cstring>
namespace RuntimeSettings {
namespace {
constexpr char TAG[] = "RuntimeSettings";
constexpr char kEventTag[] = "ProductEvent";
// 运行参数通过公共 HXC_NVS 持久化；运行期读取使用原子缓存，避免在工作线程
// 与 Shell 任务之间产生数据竞争。
// 单个表项各字段含义：
// - name    ：NVS 键名，命令行与 get/set 按此名称查找；
// - storage ：HXC_NVS 持久化句柄，承载默认值与读写；
// - value   ：运行期原子缓存，供无锁读取；
// - min/max ：允许写入范围(闭区间)，set 时据此拒绝非法值。
struct Entry {
    const char *name;
    HXC::NVS_DATA<uint32_t> storage;
    std::atomic<uint32_t> value;
    uint32_t min, max;
};
// 全部可配置运行参数的名称、默认值/初值与有效范围。单位说明：
//   connect_ms 遥控连接超时；idle_ms 空闲自动休眠；menu_idle_ms 菜单停留；
//   notice_ms 休眠前提示时长；release_ms 长按后的释放窗口；
//   off_ack_ms/off_retry_ms 关断确认与重试；on_ack_ms 开启确认；
//   fresh_ms 数据新鲜度；battery_ms 采样周期；report_ms 电量上报周期；low_mv 低电阈值。
Entry entries[] = {
    {"connect_ms", {"connect_ms", 10000}, 10000, 1000, 60000},
    {"idle_ms", {"idle_ms", 300000}, 300000, 300000, 3600000},
    {"menu_idle_ms", {"menu_idle_ms", 15000}, 15000, 5000, 300000},
    {"notice_ms", {"notice_ms", 30000}, 30000, 5000, 300000},
    {"release_ms", {"release_ms", 100}, 100, 50, 2000},
    {"off_ack_ms", {"off_ack_ms", 1000}, 1000, 500, 5000},
    {"off_retry_ms", {"off_retry_ms", 3000}, 3000, 1000, 30000},
    {"on_ack_ms", {"on_ack_ms", 5000}, 5000, 1000, 10000},
    {"fresh_ms", {"fresh_ms", 3000}, 3000, 2000, 10000},
    {"battery_ms", {"battery_ms", 30000}, 30000, 1000, 3600000},
    {"report_ms", {"report_ms", 30000}, 30000, 1000, 3600000},
    {"low_mv", {"low_mv", 3500}, 3500, 3000, 4000},
};
// “常亮”开关的持久化存储与其运行期原子镜像。
HXC::NVS_DATA<uint32_t> stored_always_on("always_on", 0);
std::atomic_bool display_always_on{false};
} // namespace
// 加载 NVS：仅接受落在 [min,max] 内的存储值，超范围或缺失时保留表内默认值。
void init() {
    if (HXC::NVS_Base::setup() != ESP_OK)
        return;
    for (auto &e : entries) {
        const uint32_t stored = e.storage.read();
        if (stored >= e.min && stored <= e.max)
            e.value.store(stored);
    }
    display_always_on.store(stored_always_on.read() != 0);
}
// 按名读取原子缓存；未知名称返回 0。
uint32_t get(const char *name) {
    for (auto &e : entries)
        if (!strcmp(name, e.name))
            return e.value.load();
    return 0;
}
// 先做范围校验并写入 NVS，成功后再更新缓存；有效变化会写入黑匣子便于审计。
bool set(const char *name, uint32_t value) {
    for (auto &e : entries)
        if (!strcmp(name, e.name)) {
            if (value < e.min || value > e.max)
                return false;
            if (e.storage.set(value) != ESP_OK) {
                ESP_LOGE(TAG, "setting write failed: %s=%lu", name, static_cast<unsigned long>(value));
                return false;
            }
            const auto old = e.value.exchange(value);
            if (old != value)
                DEVICE_EVENT_I(kEventTag, "setting %s: %lu -> %lu", name, static_cast<unsigned long>(old),
                               static_cast<unsigned long>(value));
            return true;
        }
    return false;
}
// 逐行输出 name=value [min..max]，供 shell 的 config 命令使用。
void print() {
    for (auto &e : entries)
        printf("%s=%lu [%lu..%lu]\n", e.name, static_cast<unsigned long>(e.value.load()),
               static_cast<unsigned long>(e.min), static_cast<unsigned long>(e.max));
}
// 将当前全部参数值写入黑匣子，形成一次配置快照。
void record_snapshot() {
    for (auto &e : entries)
        DEVICE_EVENT_I(kEventTag, "config %s=%lu", e.name, static_cast<unsigned long>(e.value.load()));
}
// 读取常亮开关的运行期镜像。
bool always_on() { return display_always_on.load(); }
// 持久化常亮开关并更新镜像；写入失败返回 false 且不改动运行态。
bool set_always_on(bool value) {
    if (stored_always_on.set(value ? 1U : 0U) != ESP_OK) {
        ESP_LOGE(TAG, "setting always_on write failed: %u", value ? 1U : 0U);
        return false;
    }
    DEVICE_EVENT_I(kEventTag, "setting always_on: %u -> %u", display_always_on.load() ? 1U : 0U, value ? 1U : 0U);
    display_always_on.store(value);
    return true;
}

} // namespace RuntimeSettings
