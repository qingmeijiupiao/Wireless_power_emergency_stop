#ifndef APP_RUNTIME_H
#define APP_RUNTIME_H

#include "esp_err.h"
#include "power_manager.h"

namespace AppRuntime {

/** @brief 一次启动周期内保持不变的唤醒和运行模式信息。 */
struct BootContext {
    PowerManager::WakeSource wake_source;
    bool usb_mode;
    bool process_button;
    bool button_pressed_at_boot;
};

/**
 * @brief 在 PowerManager 初始化后采集本次启动上下文
 *
 * process_button 同时考虑深睡唤醒状态和当前按键电平，覆盖冷启动时按键已经
 * 按下但没有 GPIO 唤醒标志的情况。
 */
BootContext capture_boot_context();

/** @brief 写入复位、唤醒、堆状态和固件版本诊断。 */
void append_boot_diagnostics(const BootContext& boot);

/**
 * @brief 启动本次启动周期的异步电池采样
 * @param charging 是否按维护/充电模式更新显示电量
 */
esp_err_t start_boot_battery_sample(bool charging);

/** @brief 记录无线链路已就绪及当前启动模式。 */
void log_radio_ready(const BootContext& boot);

/** @brief 记录维护模式初始化完成。 */
void log_maintenance_ready();

/** @brief 阻塞等待维护接口断开。 */
void wait_for_maintenance_disconnect();

/**
 * @brief 等待输入和按键事务结束，记录休眠快照并进入深度休眠
 *
 * 正常情况下该函数不会返回；只有深睡配置失败时才返回错误。
 */
esp_err_t sleep_when_inputs_idle();

} // namespace AppRuntime

#endif
