/**
 * @file boot_diagnostics.h
 * @brief 启动诊断接口：在开机时把复位/唤醒/电池/配置/对端等关键信息写入产品事件日志。
 */
#pragma once

// 启动诊断模块。所有输出统一走产品事件日志，便于产线/现场回溯每次启动的现场。
namespace BootDiagnostics {

/**
 * @brief 追加一条启动诊断记录
 *
 * 汇总本次启动的固件与构建信息、复位原因、唤醒原因与唤醒 GPIO、电池电压与
 * SOC、USB/按键电平、运行时配置快照以及已保存的 ESP-NOW 对端列表。仅做记录，
 * 不改变任何运行状态。
 *
 * @param release_wake 本次启动是否由“松开按键唤醒”触发，用于区分唤醒来源
 */
void append_boot(bool release_wake);

}
