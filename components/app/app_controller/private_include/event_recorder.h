/**
 * @file event_recorder.h
 * @brief 产品状态事件记录私有接口：独立维护远端状态变化的日志去重基准。
 */
#pragma once

#include "emergency_remote.h"

namespace AppController {
/** @brief 协调任务内的远端状态事件记录器，不与电量上报共享连接基准。 */
class EventRecorder {
public:
    /**
     * @brief 对比上轮快照，仅在在线、配对或保存节点身份/信道变化时记录产品事件。
     * @param remote 本轮动作执行后的远端快照。
     * @note 对象随协调任务创建，初始比较基准为 false/0；首次有效状态会记录对应变化。
     */
    void observe(const EmergencyRemote::Snapshot &remote);

private:
    bool online_before_ = false;     /**< 上轮原始 online，不受 connection_failed 影响。 */
    bool paired_before_ = false;     /**< 上轮已配对状态。 */
    bool pairing_before_ = false;    /**< 上轮正在配对状态。 */
    bool peer_before_ = false;       /**< 是否曾观察到保存节点。 */
    uint8_t mac_before_[6]{};        /**< 上次记录的节点身份，用于配对/替换回溯。 */
    uint8_t channel_before_ = 0;     /**< 上次记录的信道，不跟随周期恢复重复记录。 */
};
} // namespace AppController
