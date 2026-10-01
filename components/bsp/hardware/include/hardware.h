/**
 * @file hardware.h
 * @brief 板级硬件抽象：引脚定义、GPIO 初始化，以及休眠隔离与唤醒恢复接口。
 */
#pragma once
#include "driver/gpio.h"
#include "esp_err.h"
#include "sdkconfig.h"
// 板级 GPIO 与休眠电源管理接口，全部实现位于 hardware.cpp。
namespace Hardware {
// 用户按键输入：按下时被拉低，用于常规 UI 交互。
constexpr gpio_num_t kUiButton = GPIO_NUM_3;
// 急停按键输入：下降沿触发中断，默认由外部上拉保持高电平。
constexpr gpio_num_t kStopButton = GPIO_NUM_5;
// USB VBUS 检测输入：高电平表示 USB 已接入。
constexpr gpio_num_t kVbusDetect = GPIO_NUM_4;
// 屏幕供电使能输出：经 Q1 控制 OLED 的地回路，从而开关屏幕供电。
constexpr gpio_num_t kScreenEnable = GPIO_NUM_6;
// 电池分压使能输出：仅测量电池电压时短暂驱动，其余时间保持高阻以免漏电。
constexpr gpio_num_t kBatteryDividerEnable = GPIO_NUM_10;
/**
 * @brief 初始化板级 GPIO，并把急停处理函数注册为 IRAM 中断服务程序。
 * @param stop_handler 急停按键中断回调，须可在 IRAM 环境中运行。
 */
void init(gpio_isr_t stop_handler);
// 查询 USB 是否接入：读取 VBUS 检测引脚电平。
bool usb_connected();
// 查询用户按键是否被按下（低电平有效）。
bool button_pressed();
// 查询急停回路是否闭合（低电平有效）。
bool stop_closed();
// 控制屏幕供电使能引脚，true 打开、false 关闭。
void screen_power(bool enabled);
// 将电池分压使能引脚切为高阻并关闭数字接收器，避免分压器漏电。
void disconnect_battery_divider();
// 解除 GPIO 保持状态（深休眠保持会跨复位保留），确保后续配置能立即生效。
void release_sleep_holds();
// 配置进入休眠前所需的 GPIO 状态：隔离无关引脚并关断屏幕供电。
void configure_sleep_gpio();
// 休眠前隔离 USB PHY 引脚，降低休眠功耗并避免浮空干扰。
void isolate_usb_for_sleep();
// 休眠中止时直接恢复 USB PHY 焊盘，而不卸载仍在使用中的驱动。
void restore_usb_after_sleep_abort();
// 断开屏幕 I2C 总线引脚（SDA/SCL），避免其干扰休眠电流。
void disconnect_screen_bus();
// 唤醒后把屏幕供电使能引脚恢复为输入输出模式。
void restore_screen_pin();
// 打印板级相关 GPIO 的当前配置，便于调试。
void dump_gpio();
// 打印休眠关键 GPIO（GPIO0/6/10）的配置，便于核对隔离结果。
void dump_sleep_gpio();
} // namespace Hardware
