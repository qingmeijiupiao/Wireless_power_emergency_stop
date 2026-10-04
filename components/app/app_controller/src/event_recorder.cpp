/**
 * @file event_recorder.cpp
 * @brief 远端状态事件记录实现：仅记录变化，保持在线日志与电量上报基准独立。
 */
#include "event_recorder.h"

#include "app_controller.h"
#include "diagnostic_log.h"
#include "espnow_link.h"

namespace AppController {
void EventRecorder::observe(const EmergencyRemote::Snapshot &remote) {
    if (remote.online != online_before_)
        DEVICE_EVENT_I(kEventTag, "meter %s", remote.online ? "online" : "offline");
    if (remote.output_on != output_before_ || remote.protection_mask != protection_before_)
        DEVICE_EVENT_I(kEventTag, "meter output=%u protection=0x%02x", remote.output_on, remote.protection_mask);
    if (remote.paired != paired_before_ || remote.pairing != pairing_before_)
        DEVICE_EVENT_I(kEventTag, "pairing state paired=%u active=%u saved_peers=%u", remote.paired, remote.pairing,
                       static_cast<unsigned>(EspNowLink::get_saved_peer_count()));
    // 每轮都更新原始状态基准，connection_failed 不会制造重复的 online 日志。
    online_before_ = remote.online;
    output_before_ = remote.output_on;
    protection_before_ = remote.protection_mask;
    paired_before_ = remote.paired;
    pairing_before_ = remote.pairing;
}
} // namespace AppController
