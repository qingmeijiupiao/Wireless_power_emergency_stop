/**
 * @file battery_voltage.h
 * @brief 电池电压采集应用接口：GPIO0 ADC 分压采样、GPIO10 低边使能，含 USB 满电自动校准。
 */
#ifndef BATTERY_VOLTAGE_H
#define BATTERY_VOLTAGE_H

#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace BatteryVoltage {

// 异步采样完成回调原型。回调在采样任务上下文执行，禁止在回调内阻塞等待
// 本次采样结果；result 为采样结果，voltage_mv 为换算后的电池电压（mV），
// context 为发起采样时透传的用户指针。
using CompletionCallback = void (*)(esp_err_t result, int voltage_mv, void* context);

// 查询 USB 是否插入的回调原型。要求实现为非阻塞，校准任务在等待周期内
// 反复调用它来判断充电场景是否仍然成立。
using UsbConnectedCallback = bool (*)();

// 自动校准运行状态快照。各字段由内部状态在互斥锁保护下拷出，
// 上层仅用于显示/诊断，不要依赖其在多次读取之间保持连续。
struct CalibrationStatus {
    // 当前生效的分压倍率，Q16 定点格式
    uint32_t divider_scale_q16;
    // NVS 中持久化校准记录是否通过校验
    bool stored_calibration_valid;
    // 自动校准任务当前是否在运行
    bool monitor_running;
    // 当前稳定窗口内已累计的采样次数
    uint16_t stable_sample_count;
    // 稳定窗口内的最小电池电压（mV）
    int stable_min_mv;
    // 稳定窗口内的最大电池电压（mV）
    int stable_max_mv;
    esp_err_t last_error; /**< 最近一次校准保存错误；成功/重置后为 ESP_OK。 */
    uint32_t generation; /**< 校准窗口代际，用于诊断重置与 USB 会话。 */
};

/**
 * @brief 初始化电池采样模块
 *
 * 依次将 GPIO10 配成高阻（关闭分压路径）、初始化 GPIO0 ADC 通道、读取并
 * 校验 NVS 中的分压校准记录、创建互斥锁与完成信号量。重复调用安全。
 * 任一资源创建失败时会回滚并返回 ESP_ERR_NO_MEM。
 *
 * @param earliest_sample_us 本次启动允许开始分压采样的最早 esp_timer 时刻，单位微秒；
 *                           0 表示无需等待。此参数仅在首次初始化时设置。
 * @note 初始化不等待供电稳定；后台采样任务在使能分压路径之前等待该时刻。
 */
esp_err_t init(int64_t earliest_sample_us = 0);

/**
 * @brief 使能电池分压采样路径
 *
 * 将 GPIO10 配置为开漏输出并拉低，使分压网络低边导通。开漏输出保证
 * 模块外部无强上拉时不会与外部电路争抢电平。仅在采样窗口内调用。
 */
esp_err_t enable_sample_path();

/**
 * @brief 关闭电池分压采样路径
 *
 * 将 GPIO10 恢复为无上下拉的高阻输入，切断分压网络以降低静态功耗，
 * 避免长期导通对电池造成额外负载。
 */
esp_err_t disable_sample_path();

/**
 * @brief 启动一次异步电池电压采样
 *
 * 模块由常驻后台任务完成整个采样流程：先使能并等待 RC 分压网络稳定，
 * 再连续采样 16 次取平均，最后按当前倍率换算成电池电压。无论成功与否，
 * 请求完成前都会把 GPIO10 恢复为高阻。
 *
 * 同一时刻只允许一个采样任务，重复发起返回 ESP_ERR_INVALID_STATE。
 * 结果就绪后在后台任务上下文调用 callback（可为空），也可由其他任务通过
 * wait_mv() 获取。callback 与 wait_mv() 二者可任选其一。
 *
 * @param callback 采样完成回调，可为 nullptr
 * @param context 透传给 callback 的用户指针，可为 nullptr
 */
esp_err_t start_async(CompletionCallback callback = nullptr, void* context = nullptr);

/**
 * @brief 等待当前异步采样完成并取得结果
 *
 * 在调用入口持有当前请求的引用，结果与通知不会被下一次请求覆盖。
 * 若当前没有采样任务在运行，则立即返回最近一次的结果。尚无请求时返回
 * ESP_ERR_INVALID_STATE；慢等待者占满固定请求池时新提交返回 ESP_ERR_NO_MEM。
 *
 * @note wait_mv 捕获调用入口的当前请求；需要绑定自己发起的请求时使用 read_mv 或回调。
 * @param voltage_mv 输出电池电压，单位 mV
 * @param ticks_to_wait 最大等待时间；超时返回 ESP_ERR_TIMEOUT
 */
esp_err_t wait_mv(int& voltage_mv, TickType_t ticks_to_wait = portMAX_DELAY);

/** @brief 禁止新采样/校准保存，等待当前 GPIO 操作及回调结束；失败后仍暂停。
 * @param ticks_to_wait 等待当前采样静止的最大时间。
 * @return true 可隔离 GPIO；false 必须中止睡眠并调用 cancel_sleep()。
 */
bool prepare_sleep(TickType_t ticks_to_wait = pdMS_TO_TICKS(250));
/** @brief 取消睡眠握手，重新接受采样请求。 */
void cancel_sleep();
/** @brief 产品协调任务发布每次 USB 边沿，使拔插之间的旧校准窗口失效。 */
void notify_external_power(bool connected);

/** @brief 当前有异步采样任务运行时返回 true。未初始化时返回 false。 */
bool is_busy();

/**
 * @brief 同步方式读取一次电池电压
 *
 * 便捷封装：提交与持有请求引用在同一事务中完成，因此会阻塞调用者直到自己的本次
 * 采集完成，仅适合允许阻塞的上下文。
 */
esp_err_t read_mv(int& voltage_mv);

/**
 * @brief 启动 USB 满电电压自动校准任务
 *
 * 在 USB 持续连接且电池电压高于 4.0 V 时，每 10 秒采样一次。连续 10 分钟
 * （约 60 个样本）内电压极差小于 5 mV，即认为充电已稳定在 4.2 V 参考电压，
 * 据此反推新的分压倍率并写入 NVS。成功后本插电会话退出；失败最多尝试
 * 三次，每次重收完整稳定窗口，last_error 发布错误。再次 USB 插入重新启动监测。
 *
 * 分压倍率是硬件参数，长期受元件偏差影响，自动校准用于弥补该偏差；
 * reset 与 USB 边沿变更校准代际，旧窗口及旧保存事务同时失效。
 *
 * @param usb_connected 查询 USB 插入状态的无阻塞回调，不可为空
 */
esp_err_t start_calibration_monitor(UsbConnectedCallback usb_connected);

/** @brief 读取当前分压倍率和自动校准任务状态快照。 */
void get_calibration_status(CalibrationStatus& status);

/**
 * @brief 清除已保存的校准结果并恢复默认 2.0 倍分压倍率
 *
 * 写入一条校验无效的记录，使系统在重启后仍显示为“未校准”并使用默认倍率。
 */
esp_err_t reset_calibration();

} // namespace BatteryVoltage

#endif
