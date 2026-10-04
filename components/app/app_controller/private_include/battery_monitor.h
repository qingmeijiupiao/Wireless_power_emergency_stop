/**
 * @file battery_monitor.h
 * @brief 产品电池策略私有接口：采样结果交接、周期调度、USB 事件与电量上报。
 */
#pragma once

#include "emergency_remote.h"
#include "esp_err.h"
#include "app_diagnostics.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace AppController {

/** 提供给界面和事件日志的电池值副本。 */
struct BatteryView {
    int voltage_mv = 0; /**< 电池电压，单位 mV；0 表示尚无有效读数。 */
    int percent = -1;   /**< 显示电量，范围 0..100；-1 表示未知，不应上报。 */
};

/**
 * @brief 管理产品侧的电池采样与上报策略。
 * @note init() 在启动任务中调用；其余运行接口由协调任务串行调用。
 *       完成回调只写线程安全邮箱；对象必须存活至全部异步回调结束。
 */
class BatteryMonitor {
public:
    /** 本轮电池处理对界面和静置计时的影响。 */
    struct Update {
        bool redraw = false;   /**< 请求界面重绘；周期采样不会计为用户活动。 */
        bool activity = false; /**< USB 插拔发生变化，应刷新静置计时。 */
    };

    /**
     * @brief 建立单槽邮箱并发起首次异步采样，不等待采样结果。
     * @pre BatteryVoltage 和 BatteryStatus 已初始化，协调任务尚未启动。
     * @retval ESP_OK 邮箱创建成功；采样启动错误已入队，后续由 poll() 处理。
     * @retval ESP_ERR_INVALID_STATE 本对象的邮箱已经创建。
     * @retval ESP_ERR_NO_MEM 邮箱创建失败。
     */
    esp_err_t init();
    /**
     * @brief 初始化周期采样基准，插电启动时同时请求后台校准监测。
     * @param now_us 当前 esp_timer 时间，单位微秒。
     * @param usb 当前 USB 供电状态。
     * @pre init() 已成功；由协调任务在第一次 poll() 前调用一次。
     */
    void begin(int64_t now_us, bool usb);
    /**
     * @brief 消费最新采样结果、检测 USB 边沿并调度到期采样。
     * @param now_us 当前 esp_timer 时间，单位微秒。
     * @param usb 本轮读取的 USB 供电状态，同时用作电量更新的充电标志。
     * @return 本轮的重绘与活动标志。
     * @note 邮箱读取不等待；电量更新和日志调用仍可能等待各自服务的同步资源。
     *       首次成功前采样失败按 1 秒安排重试；采样忙时跳过本轮并顺延周期。
     */
    Update poll(int64_t now_us, bool usb);
    /**
     * @brief 读取电池显示值副本。
     * @return 优先使用 BatteryStatus 中的平滑电量（含有效 RTC 恢复值）；
     *         无有效电量时返回最近成功采样电压和未知百分比。
     */
    BatteryView view() const;
    /**
     * @brief 在有效连接上按周期上报电量，重连后立即补报。
     * @param remote 本轮动作执行后的远端快照。
     * @param percent 显示电量，有效范围 0..100；负数表示未知，跳过上报。
     * @param now_us 当前 esp_timer 时间，单位微秒。
     * @note 仅在 online 且未 connection_failed 时上报；只提交无线请求，不等待确认。
     */
    void report(const EmergencyRemote::Snapshot &remote, int percent, int64_t now_us);

private:
    /** 采样任务到协调任务的单槽邮箱载荷。 */
    struct Sample {
        esp_err_t result; /**< 采样结果或异步启动错误码。 */
        int voltage_mv;   /**< 成功时有效的电池电压，单位 mV。 */
    };
    /**
     * @brief 非阻塞发布采样结果，不访问 UI 或电量估算状态。
     * @param result 采样结果或异步启动错误码。
     * @param voltage_mv 成功采样电压，单位 mV；失败时不使用。
     * @param context 发起采样的 BatteryMonitor 指针，必须非空且保持有效。
     * @note 通常在采样任务中执行；启动失败时由 request_sample() 同步调用。
     */
    static void on_sample(esp_err_t result, int voltage_mv, void *context);
    /** @brief 发起异步采样；启动失败也写入邮箱，统一由 poll() 处理。 */
    void request_sample();

    QueueHandle_t samples_ = nullptr; /**< 长度为 1 的结果邮箱，回调可覆盖未消费的旧结果。 */
    int latest_mv_ = 0;               /**< 最近成功采样电压（mV），仅由协调任务更新。 */
    int64_t sample_at_ = 0;           /**< 下次周期采样或首次失败重试的截止时间（微秒）。 */
    int64_t report_at_ = 0;           /**< 下次周期电量上报的截止时间（微秒）。 */
    bool boot_pending_ = true;        /**< 尚未取得首次成功采样，用于重试和启动事件补记。 */
    bool usb_before_ = false;         /**< 上轮 USB 状态，仅用于插拔边沿检测。 */
    bool connected_before_ = false;   /**< 上轮 online && !connection_failed，仅用于重连补报。 */
    bool low_before_ = false;         /**< 低电进入/退出事件基准，不跟随 UI 提示页刷新。 */
    AppDiagnostics::ErrorLog sample_log_; /**< 周期采样失败只在错误变化时持久化。 */
};
} // namespace AppController
