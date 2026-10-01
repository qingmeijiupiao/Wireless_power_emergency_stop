/**
 * @file shell_command.h
 * @brief 急停控制器 Shell 命令注册模块，集中管理电池、遥控、配置和黑匣子命令
 */
#ifndef SHELL_COMMAND_H
#define SHELL_COMMAND_H

#include "esp_err.h"

namespace ShellCommand {

/**
 * @brief 初始化 Shell 并注册急停应用命令
 *
 * 注册 `version`、`battery`、`config`、`set`、`remote`、`gpio`、`fault`
 * 和 `blackbox` 命令。命令层只负责参数解析、输出和调用应用服务，不持有
 * ESP-NOW 请求状态，也不依赖运行任务派发。
 *
 * @note 仅在检测到 USB 插入后调用。调用前必须完成 BatteryVoltage、
 *       EmergencyRemote 和黑匣子服务初始化。
 * @return ESP_OK 初始化成功，其他值来自 Shell::init()
 */
esp_err_t init();

} // namespace ShellCommand

#endif
