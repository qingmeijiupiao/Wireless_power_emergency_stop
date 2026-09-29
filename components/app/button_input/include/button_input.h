/*
 * @version: 2.0
 * @LastEditors: qingmeijiupiao
 * @Description: 遥控按键低功耗事务接口
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-07
 */
#ifndef BUTTON_INPUT_H
#define BUTTON_INPUT_H

#include <cstdint>

#include "esp_err.h"

namespace ButtonInput {

/**
 * @brief 在系统慢初始化前启动本次唤醒按键跟踪
 *
 * 按下时立即点灯；达到 1 秒立即三闪并排队发送 ON；提前释放则立即
 * 单闪并排队发送 OFF。无线尚未就绪时发送任务会等待 ready 通知。
 */
esp_err_t start_wakeup_tracking();

/** @brief 通知按键命令任务 ESP-NOW 已初始化，可以发送。 */
void notify_radio_ready();

/**
 * @brief USB 供电模式下启动 GPIO4 常驻按键任务
 *
 * 短按发送 OFF，按住至少 1 秒发送 ON。
 */
esp_err_t init_usb_mode();

/** @brief 按键采集或同步 ESP-NOW 请求执行期间返回 true。 */
bool is_busy();

} // namespace ButtonInput

#endif
