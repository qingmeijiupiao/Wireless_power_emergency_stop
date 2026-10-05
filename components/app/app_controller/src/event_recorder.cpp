/**
 * @file event_recorder.cpp
 * @brief 远端状态事件记录实现：仅记录变化，保持在线日志与电量上报基准独立。
 */
#include "event_recorder.h"

#include "app_controller.h"
#include "app_diagnostics.h"
#include "espnow_link.h"
#include <cstring>

namespace AppController {
void EventRecorder::observe(const EmergencyRemote::Snapshot &remote) {
    EspNowLink::PairingResult result = {};
    EspNowLink::get_pairing_result(&result);
    if (result.serial != pairing_result_before_) {
        pairing_result_before_ = result.serial;
        APP_LOGI(kEventTag, "pair result=%s peer=%02x:%02x:%02x:%02x:%02x:%02x ch=%u legacy=%u",
                 esp_err_to_name(result.error), result.peer.bytes[0], result.peer.bytes[1], result.peer.bytes[2],
                 result.peer.bytes[3], result.peer.bytes[4], result.peer.bytes[5], result.channel, result.legacy);
    }
    if (remote.online != online_before_)
        APP_LOGI(kEventTag, "meter %s", remote.online ? "online" : "offline");
    if (remote.paired != paired_before_ || remote.pairing != pairing_before_)
        APP_LOGI(kEventTag, "pairing state paired=%u active=%u saved_peers=%u", remote.paired, remote.pairing,
                       static_cast<unsigned>(EspNowLink::get_saved_peer_count()));
    // 每轮都更新原始状态基准，connection_failed 不会制造重复的 online 日志。
    online_before_ = remote.online;
    paired_before_ = remote.paired;
    pairing_before_ = remote.pairing;
    // 保存节点/信道变化附带身份；不扩大底层 INFO 白名单，避免带入恢复轮询日志。
    EspNowLink::SavedPeer peer{};
    const bool has_peer = EspNowLink::get_saved_peer(0, &peer) == ESP_OK;
    if (has_peer && (!peer_before_ || channel_before_ != peer.last_channel ||
                     memcmp(mac_before_, peer.address.bytes, sizeof(mac_before_)) != 0)) {
        APP_LOGI(kEventTag, "peer saved mac=%02x:%02x:%02x:%02x:%02x:%02x channel=%u source=%s",
                 peer.address.bytes[0], peer.address.bytes[1], peer.address.bytes[2], peer.address.bytes[3],
                 peer.address.bytes[4], peer.address.bytes[5], peer.last_channel, peer_before_ ? "changed" : "loaded_or_paired");
        memcpy(mac_before_, peer.address.bytes, sizeof(mac_before_));
        channel_before_ = peer.last_channel;
    }
    if (peer_before_ && !has_peer)
        APP_LOGI(kEventTag, "peer saved result=removed");
    peer_before_ = has_peer;
}
} // namespace AppController
