/**
 * @file ui_state.h
 * @brief UI 跨页面共享状态：菜单、故障提示、消息与低电提示等。
 */
#pragma once
#include "emergency_remote.h"
#include "ui_policy.h"
namespace EmergencyUi {
/** 由 UiManager 持有、各 Page 读写的共享 UI 状态。 */
struct UiState {
    UiPolicy::Menu menu;                 /**< 菜单状态容器 */
    UiPolicy::FaultNotice notice;        /**< 故障提示状态机 */
    int message = 0;                     /**< 当前消息编号 */
    bool fault_acknowledged = false;     /**< 本轮故障是否已被用户确认 */
    bool failed_before = false;          /**< 上一周期是否连接失败，用于边沿检测 */
    bool controlling = false;            /**< 远端正处于停止/启动过程 */
    bool low_notified = false;           /**< 本轮低电是否已提示 */
    int64_t low_notice_until = 0;        /**< 低电提示保持截止时刻 */
    int fault_page = -1;                 /**< 最近提示的故障页编号 */
    int current_fault = -1;              /**< 当前实时故障页编号 */
    int64_t state_since = 0;             /**< 当前远端状态进入时刻 */
    EmergencyRemote::State last_state = EmergencyRemote::State::UNPAIRED; /**< 上一周期远端状态 */
};
} // namespace EmergencyUi
