/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 遥控器 Shell 命令注册模块，集中管理链路、远程控制和黑匣子命令
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-06
 */
#ifndef SHELL_COMMAND_H
#define SHELL_COMMAND_H

#include "esp_err.h"

namespace ShellCommand {

/**
 * @brief 初始化 Shell 并注册遥控器应用命令
 *
 * 注册 `battery`、`espnow`、`remote` 和 `blackbox` 命令。命令层只负责参数解析、
 * 输出和调用应用服务，不持有 ESP-NOW 请求状态。
 *
 * @note 仅在检测到 USB 插入后调用。调用前必须完成 BatteryVoltage、
 *       EspNowRemote、StatusLed 和 BlackboxService 初始化。
 * @return ESP_OK 初始化成功，其他值来自 Shell::init()
 */
esp_err_t init();

} // namespace ShellCommand

#endif
