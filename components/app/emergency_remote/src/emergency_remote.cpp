/**
 * @file emergency_remote.cpp
 * @brief 急停遥控从机实现：独立工作线程、急停 ISR 锁存、ON/OFF 事务与 ACK 超时重试、
 *        信道恢复、配对状态机、数据快照与睡前握手。
 */
#include "emergency_remote.h"
#include "espnow_service.h"
#include "runtime_settings.h"
#include "app_diagnostics.h"
#include "wifi_manager.h"
#include "espnow_codec.h"
#include "hardware.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <atomic>

namespace EmergencyRemote {
namespace {
constexpr char TAG[] = "EmergencyRemote";
constexpr char kEventTag[] = "ProductEvent";
// 详细应答消息的类型标识：负载依次为 id、动作、结果、输出、原因码、保护位。
constexpr uint16_t kDetailMessage = 0x0203;
// 保护 model 与 controller 的自旋锁：ISR、工作线程与 UI 线程都会访问这些共享数据。
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
Snapshot model{};
EspNowLink::MacAddress controller{};
// 来自 ISR/任务上下文的原子请求标志：ISR 只能置位，统一由工作线程消费。
std::atomic_bool contact_fell{false}, stop_requested{false}, on_requested{false}, pair_requested{false};
std::atomic_bool wrong_channel_requested{false}, delete_requested{false}, pause_requested{false};
std::atomic_bool retry_requested{false};
// 启动时是否处于“触点释放唤醒恢复”场景，影响首次 ON 的排队条件。
bool resume_release_on_boot = false;
// 工作线程与异步回调之间传递的开关应答记录（简化或详细两种来源）。
struct Response {
    uint32_t id = 0, serial = 0;
    EspNowService::SwitchAction action{};
    EspNowService::SwitchResult result{};
    bool output = false;
    uint8_t reason = 10, mask = 0; // reason 默认 10 表示“无详细原因”
};
QueueHandle_t responses = nullptr;
struct Telemetry {
    EspNowService::DeviceData data{};
    int64_t received_at = 0;
    uint32_t session = 0, request_id = 0;
};
QueueHandle_t telemetry = nullptr;
uint32_t peer_session = 1;
uint32_t current_data_request = 0; // 仅工作线程访问；用于匹配当前遥测请求。

// 非阻塞入队：队列满时丢弃应答并记录错误，使 OFF 事务保持待处理以便重试。
void enqueue(const Response &rsp) {
    if (xQueueSend(responses, &rsp, 0) != pdTRUE)
        APP_LOGE(TAG, "response queue full; OFF remains pending");
}

// 鉴权：仅当已配对且来源 MAC 等于已保存控制端时才接受报文，避免伪造或串扰。
bool accept_peer(const EspNowLink::MacAddress &source, uint32_t& session) {
    portENTER_CRITICAL(&lock);
    const bool accept = model.paired && source == controller;
    session = peer_session;
    portEXIT_CRITICAL(&lock);
    return accept;
}

// 精简开关应答回调：鉴权通过后打包成 Response 交给工作线程处理。
void receive_switch(const EspNowLink::MacAddress &source, uint32_t id, EspNowService::SwitchAction action,
                    EspNowService::SwitchResult result, bool output, void *) {
    Response rsp{};
    if (!accept_peer(source, rsp.serial)) return;
    rsp.id = id;
    rsp.action = action;
    rsp.result = result;
    rsp.output = output;
    enqueue(rsp);
}

// 详细开关应答回调：校验可靠性、长度与各字段取值域后解析成 Response。
void receive_detail(const EspNowLink::Message &message, void *) {
    Response rsp{};
    if (!message.reliable || message.payload_size != 9 || !accept_peer(message.source, rsp.serial) || message.payload[4] > 2 ||
        message.payload[5] > 4 || message.payload[6] > 1 || message.payload[7] > 10 || message.payload[8] > 15)
        return;
    const uint32_t id = EspNowLink::Codec::load_le<uint32_t>(message.payload);
    if (!id)
        return;
    rsp.id = id;
    rsp.action = static_cast<EspNowService::SwitchAction>(message.payload[4]);
    rsp.result = static_cast<EspNowService::SwitchResult>(message.payload[5]);
    rsp.output = message.payload[6] != 0;
    rsp.reason = message.payload[7];
    rsp.mask = message.payload[8];
    enqueue(rsp);
}

// 遥测回调只覆盖最新值邮箱，控制字段与安全证据仅由工作者更新。
void receive_data(const EspNowLink::MacAddress &source, uint32_t request_id, const EspNowService::DeviceData &data, bool available,
                  bool, void *) {
    Telemetry event{};
    if (!available || !accept_peer(source, event.session)) return;
    event.data = data;
    event.request_id = request_id;
    event.received_at = esp_timer_get_time();
    xQueueOverwrite(telemetry, &event);

}

// 把远端拒绝原因码映射为对应状态；未知原因按结果兜底。
State refusal_state(const Response &rsp) {
    switch (rsp.reason) {
    case 1:
        return State::SHORT_FAULT;
    case 2:
        return State::PROTECTED;
    case 3:
        return State::COOLDOWN;
    case 4:
        return State::BUSY;
    case 6:
        return State::NOT_READY;
    case 7:
        return State::DETECT_ERROR;
    default:
        return rsp.result == EspNowService::SwitchResult::NOT_READY ? State::NOT_READY : State::REJECTED;
    }
}
/** @brief 原因由远端应答提供；没有详细应答时明确标为 unknown。 */
const char* reason_name(uint8_t reason) {
    switch (reason) {
    case 0: return "none";
    case 1: return "short_fault";
    case 2: return "protection";
    case 3: return "cooldown";
    case 4: return "busy";
    case 6: return "not_ready";
    case 7: return "detect_error";
    case 10: return "unknown";
    default: return "other";
    }
}
/** 一次操作跨多个报文重试使用同一编号；这些字段仅用于记录，不驱动控制策略。 */
struct OperationLog {
    uint32_t number = 0, attempts = 0;
    int64_t started_us = 0;
    uint8_t reason = 255, result = 255;
    bool finished = true;
};
/** @brief 建立操作日志上下文，并明确记录未完成操作被新请求替代。 */
void begin_operation(OperationLog& operation, uint32_t& serial, const char* action, const char* source, int64_t now) {
    if (!operation.finished)
        APP_LOGI(kEventTag, "operation op=%lu action=%s result=superseded attempts=%lu",
                 static_cast<unsigned long>(operation.number), action, static_cast<unsigned long>(operation.attempts));
    operation = {++serial, 0, now, 255, 255, false};
    APP_LOGI(kEventTag, "operation op=%lu action=%s source=%s result=requested",
             static_cast<unsigned long>(operation.number), action, source);
}
/** @brief 操作结束记录；总超时与应答成功使用不同结果名称，不能混淆关闭确认。 */
void finish_operation(OperationLog& operation, const char* action, const char* result, int64_t now) {
    if (operation.finished) return;
    APP_LOGI(kEventTag, "operation op=%lu action=%s result=%s attempts=%lu elapsed_ms=%lld",
             static_cast<unsigned long>(operation.number), action, result,
             static_cast<unsigned long>(operation.attempts), static_cast<long long>((now - operation.started_us) / 1000));
    operation.finished = true;
}

// 在临界区内更新对外状态。
void set_state(State state, const char* cause = "control") {
    portENTER_CRITICAL(&lock);
    const State previous = model.state;
    model.state = state;
    portEXIT_CRITICAL(&lock);
    // 在状态所有者处记录每次实际变化；不等待 UI 的下一次采样，且不在临界区输出。
    if (previous != state)
        APP_LOGI(kEventTag, "state %s -> %s cause=%s", state_name(previous), state_name(state), cause);
}

// 使已有的“静止”结论失效：出现新事件时禁止进入睡眠。
void invalidate_sleep() {
    portENTER_CRITICAL(&lock);
    model.busy = true;
    model.quiesced = false;
    portEXIT_CRITICAL(&lock);
}
// 独立控制工作线程：以 10ms 节拍消费请求、推进状态机、收发开关与遥测并维护快照。
void worker(void *) {
    bool was_low = Hardware::stop_closed();
    // release_armed：触点曾闭合，需等待稳定释放后才允许 ON；on_after_off：已排队一次 ON。
    bool release_armed = was_low || resume_release_on_boot, on_after_off = false;
    // 释放唤醒的 OFF 是恢复开启前的内部同步；界面展示用户的开启意图。
    // 新 STOP/重试/配对会结束此启动上下文，不能掩盖随后真正的关断。
    bool release_resume = resume_release_on_boot && !was_low;
    bool off_required = true; // 重启/配对后始终先同步 OFF，启动时绝不自动开输出。
    bool pending = false, recovery_needed = false;
    uint32_t id = 0;
    auto action = EspNowService::SwitchAction::OFF;
    // deadline=应答超时；retry_at=下次可发送；high_since=触点释放起点；
    // data_at=下次遥测轮询；recover_at=下次信道恢复。
    int64_t deadline = 0, retry_at = 0, high_since = 0, data_at = 0, recover_at = 0;
    int64_t connection_since = esp_timer_get_time();
    bool failed = false;     // 连接重试是否已耗尽
    int64_t off_since = connection_since; // 独立关断期限，遥测/重试均不能续期。
    int64_t on_submit_until = 0;
    int64_t protection_refused_at = 0; // 旧遥测不能立即抹掉本轮保护拒绝。
    bool maybe_on = false; // ON 入队后，旧 OFF 遥测不能清除此锁存。
    bool off_logged = false; // 同一轮 OFF 业务事件只记录一次，避免刷屏
    EspNowLink::LinkStatistics last_stats{};
    int64_t stats_at = 0;
    uint32_t operation_serial = 0;
    OperationLog off_operation, on_operation;
    begin_operation(off_operation, operation_serial, "OFF", "boot_sync", connection_since);
    if (release_resume)
        set_state(State::STARTING, "release_wake");
    AppDiagnostics::ErrorLog radio_log, submit_log, data_log, recovery_log, recovery_submit_log;
    bool recovery_pending = false;
    uint8_t protection_before = 0;
    bool output_before = false, confirmed_before = false;
    uint32_t pair_result_serial = 0;
    int64_t interlock_at = 0;
    bool interlock_before = true;
    while (true) {
        const int64_t now = esp_timer_get_time();
        const bool low = Hardware::stop_closed();
        EspNowLink::PairingResult pairing_result = {};
        EspNowLink::get_pairing_result(&pairing_result);
        if (pairing_result.serial != pair_result_serial) {
            pair_result_serial = pairing_result.serial;
            if (pairing_result.initiator) {
                // 配对结束后重建输出证据，旧事务结果不能作用到新绑定。
                portENTER_CRITICAL(&lock);
                ++peer_session;
                model.output_confirmed = false;
                model.output_time_us = 0;
                model.data_time_us = 0;
                portEXIT_CRITICAL(&lock);
                pending = false;
                id = 0;
                current_data_request = 0;
                off_required = true;
                off_since = now;
                connection_since = now;
                failed = false;
                on_after_off = false;
                release_armed = false;
                release_resume = false;
                retry_at = 0;
                interlock_at = 0;
                EspNowLink::cancel_transmissions();
            }
        }
        // 每秒汇总一次链路统计增量，仅在有异常时告警，便于诊断无线质量。
        if (now >= stats_at) {
            EspNowLink::LinkStatistics stats{};
            EspNowLink::get_statistics(&stats);
            const uint32_t submit = stats.tx_submit_errors - last_stats.tx_submit_errors;
            const uint32_t mac = stats.tx_mac_failures - last_stats.tx_mac_failures;
            const uint32_t ack = stats.ack_timeouts - last_stats.ack_timeouts;
            const uint32_t invalid = stats.rx_invalid_packets - last_stats.rx_invalid_packets;
            const uint32_t overflow = stats.rx_queue_overflows - last_stats.rx_queue_overflows;
            if (submit || mac || ack || invalid || overflow)
                APP_LOGI(TAG,
                         "ESPNOW delta submit=%lu mac=%lu ack_timeout=%lu rx_invalid=%lu rx_overflow=%lu",
                         static_cast<unsigned long>(submit), static_cast<unsigned long>(mac),
                         static_cast<unsigned long>(ack), static_cast<unsigned long>(invalid),
                         static_cast<unsigned long>(overflow));
            last_stats = stats;
            stats_at = now + 1000000;
        }
        // 删除配对：清空本机绑定后停在未配对状态，不再向原对端发包。
        if (delete_requested.exchange(false)) {
            EspNowLink::leave_pairing_mode();
            const esp_err_t err = EspNowLink::clear_saved_peers();
            EspNowLink::cancel_transmissions();
            portENTER_CRITICAL(&lock);
            controller = {};
            ++peer_session;
            model.output_confirmed = false;
            model.output_time_us = 0;
            model.data_time_us = 0;
            portEXIT_CRITICAL(&lock);
            pending = false;
            id = 0;
            on_after_off = false;
            maybe_on = false;
            off_required = false;
            failed = false;
            recovery_needed = false;
            release_armed = false;
            release_resume = false;
            current_data_request = 0;
            retry_at = recover_at = data_at = 0;
            set_state(State::UNPAIRED, "pairing_deleted");
            APP_LOGI(kEventTag, "pairing deleted peers result=%s", esp_err_to_name(err));
        }
        if (wrong_channel_requested.exchange(false)) {
            APP_LOGI(kEventTag, "channel test source=shell channel=6 result=%s", esp_err_to_name(WiFiManager::instance().set_channel(6)));
        }
        // 消费本拍到来的急停/停止/重试请求；exchange 保证每个事件只被处理一次。
        const bool fall = contact_fell.exchange(false);
        const bool stop = stop_requested.exchange(false);
        if (retry_requested.exchange(false)) {
            release_resume = false;
            finish_operation(on_operation, "ON", "cancelled_by_retry", now);
            begin_operation(off_operation, operation_serial, "OFF", "user_retry", now);
            invalidate_sleep();
            failed = false;
            connection_since = now;
            retry_at = recover_at = 0;
            pending = false;
            id = 0;
            recovery_needed = true;
            off_required = true;
            off_since = now;
            EspNowLink::cancel_transmissions();
            current_data_request = 0;
            on_after_off = false;
            APP_LOGI(TAG, "USER_RETRY window_ms=%lu", static_cast<unsigned long>(RuntimeSettings::get(RuntimeSettings::Id::ConnectMs)));
            APP_LOGI(kEventTag, "connection retry requested");
            off_logged = false;
        }
        // 急停锁存：无论是 ISR 边沿、任务请求还是轮询首次发现的闭合，都强制进入 OFF
        // 事务并作废睡眠，同时清除尚未发送的 ON。
        if (stop || fall || (low && !was_low)) {
            release_resume = false;
            finish_operation(on_operation, "ON", "cancelled_by_stop", now);
            begin_operation(off_operation, operation_serial, "OFF", stop ? "software_stop" : "stop_contact", now);
            invalidate_sleep();
            failed = false;
            connection_since = now;
            off_required = true;
            off_since = now;
            EspNowLink::cancel_transmissions();
            current_data_request = 0;
            on_after_off = false;
            release_armed = low || fall;
            high_since = low ? 0 : now;
            pending = false;
            id = 0;
            retry_at = 0;
            set_state(State::STOPPING, stop ? "software_stop" : "stop_contact");
            APP_LOGI(kEventTag, "STOP latched contact=%d edge=%u requested=%u", low, fall, stop);
            off_logged = false;
        }
        // 触点仍闭合则持续压制释放计时；只有高电平保持超过 release_ms 才认定释放稳定。
        if (low)
            high_since = 0;
        else {
            if (!high_since)
                high_since = now;
            if (release_armed && now - high_since >= RuntimeSettings::get(RuntimeSettings::Id::ReleaseMs) * 1000LL) {
                release_armed = false;
                if (!failed) {
                    begin_operation(on_operation, operation_serial, "ON", "stop_released", now);
                    on_after_off = true;
                    on_submit_until = now + RuntimeSettings::get(RuntimeSettings::Id::OnAckMs) * 1000LL;
                }
                APP_LOGI(TAG, "RELEASE_STABLE; ON waits for confirmed OFF");
                APP_LOGI(kEventTag, "STOP released stable ON_queued=%u reason=%s", !failed,
                         failed ? "connection_failed" : "wait_for_confirmed_OFF");
            }
        }
        was_low = low;
        // 诊断用 ON 请求：触点闭合或释放尚未稳定时一律忽略，防止越过安全约束。
        const bool on_request = on_requested.exchange(false);
        if (on_request && (low || release_armed))
            APP_LOGI(kEventTag, "ON source=shell result=denied reason=%s", low ? "stop_held" : "release_unstable");
        if (on_request && !low && !release_armed) {
            begin_operation(on_operation, operation_serial, "ON", "shell", now);
            invalidate_sleep();
            failed = false;
            connection_since = now;
            on_after_off = true;
            on_submit_until = now + RuntimeSettings::get(RuntimeSettings::Id::OnAckMs) * 1000LL;
            if (!snapshot().output_confirmed) {
                begin_operation(off_operation, operation_serial, "OFF", "before_unconfirmed_ON", now);
                off_required = true; off_since = now;
            }
            APP_LOGI(kEventTag, "ON requested");
        }
        // 链路未激活（例如掉线）时，按已保存节点的信道重新拉起 STA 无线。
        if (!failed && !EspNowLink::is_active()) {
            EspNowLink::SavedPeer peer{};
            const uint8_t channel = EspNowLink::get_saved_peer(0, &peer) == ESP_OK ? peer.last_channel : 1;
            const auto err = WiFiManager::instance().start_sta_radio(channel);
            radio_log.observe(TAG, "radio resume", err);
            if (err == ESP_OK)
                APP_LOGI(kEventTag, "radio resumed channel=%u", channel);
        }
        // 配对请求：无待办事务、未在恢复信道/配对中即受理；已配对时也允许重新配对，成功才替换旧绑定。
        if (pair_requested.exchange(false)) {
            invalidate_sleep();
            if (!pending && !on_after_off && !EspNowLink::is_recovering_channel() && !EspNowLink::is_pairing()) {
                failed = false;
                connection_since = now;
                if (off_required) off_since = now;
                if (!EspNowLink::is_active())
                    WiFiManager::instance().start_sta_radio(1);
                const auto err = EspNowLink::start_pairing();
                APP_LOGI(kEventTag, "pairing start: %s", esp_err_to_name(err));
            } else
                APP_LOGI(kEventTag, "pairing denied: pending transaction or busy");
        }
        // 最新遥测只更新显示；未知 ON 的安全证据只能由匹配的控制应答纠正。
        Telemetry incoming{};
        if (xQueueReceive(telemetry, &incoming, 0) == pdTRUE) {
            portENTER_CRITICAL(&lock);
            if (incoming.session == peer_session) {
                const auto& data = incoming.data;
                model.data = {data.voltage_mv, data.current_ua, data.board_temperature_centi_c,
                    data.chip_temperature_centi_c, data.charge_uah, data.energy_uwh,
                    data.meter_time_ms, data.status_flags};
                model.data_time_us = incoming.received_at;
                model.protection_mask = (incoming.data.status_flags >> 1) & 15;
                if (!pending && !off_required && !maybe_on && incoming.request_id &&
                    incoming.request_id == current_data_request) {
                    model.output_on = (incoming.data.status_flags & 1) != 0;
                    model.output_time_us = incoming.received_at;
                }
            }
            portEXIT_CRITICAL(&lock);
        }
        // 排空应答队列，只采纳与当前在途事务 id/动作匹配的应答。
        Response rsp{};
        while (xQueueReceive(responses, &rsp, 0) == pdTRUE) {
            if (id && rsp.id == id && rsp.action == action && rsp.serial == peer_session) {
                connection_since = now; // 收到有效业务应答即证明对端可达。
                const bool first_response = pending;
                pending = false;
                portENTER_CRITICAL(&lock);
                model.output_on = rsp.output;
                model.output_confirmed = true;
                maybe_on = false;
                model.output_time_us = now;
                // 精简应答不会覆盖由详细应答建立的更新的保护原因。
                if (rsp.reason != 10 || rsp.result == EspNowService::SwitchResult::OK)
                    model.protection_mask = rsp.mask;
                portEXIT_CRITICAL(&lock);
                // 成功必须同时满足“结果 OK”且“输出状态与请求动作一致”。
                const bool success = rsp.result == EspNowService::SwitchResult::OK &&
                                     rsp.output == (action == EspNowService::SwitchAction::ON);
                auto& operation = action == EspNowService::SwitchAction::ON ? on_operation : off_operation;
                if (success && first_response)
                    APP_LOGI(kEventTag, "switch ACK op=%lu id=%lu action=%u output=%u confirmed=1",
                             static_cast<unsigned long>(operation.number), static_cast<unsigned long>(id),
                             static_cast<unsigned>(action), rsp.output);
                if (!success && (operation.reason != rsp.reason || operation.result != static_cast<uint8_t>(rsp.result))) {
                    APP_LOGI(kEventTag, "switch refused op=%lu id=%lu action=%u result=%u reason=%s(%u) protect=0x%x",
                             static_cast<unsigned long>(operation.number), static_cast<unsigned long>(id),
                             static_cast<unsigned>(action), static_cast<unsigned>(rsp.result),
                             reason_name(rsp.reason), rsp.reason, rsp.mask);
                    operation.reason = rsp.reason; operation.result = static_cast<uint8_t>(rsp.result);
                }
                if (success || action == EspNowService::SwitchAction::ON)
                    finish_operation(operation, action == EspNowService::SwitchAction::ON ? "ON" : "OFF",
                                     success ? "confirmed" : "refused", now);
                APP_LOGI(TAG, "SWITCH_ACK id=%lu action=%u result=%u output=%u reason=%u protect=%u",
                         static_cast<unsigned long>(id), static_cast<unsigned>(action),
                         static_cast<unsigned>(rsp.result), rsp.output, rsp.reason, rsp.mask);
                // OFF 失败需继续重试并保持 off_required；ON 仅在不需要关断时才切换状态。
                if (action == EspNowService::SwitchAction::OFF) {
                    off_required = !success;
                    retry_at = success ? 0 : now + 100000;
                    set_state(success ? (release_resume ? State::STARTING : State::OFF) : State::OFFLINE,
                              success ? "OFF_ack" : "OFF_refused");
                } else if (!off_required) {
                    if (rsp.reason != 10 || success || first_response) {
                        const State result_state = success ? State::ON : refusal_state(rsp);
                        if (result_state == State::PROTECTED) protection_refused_at = now;
                        set_state(result_state, success ? "ON_ack" : reason_name(rsp.reason));
                    }
                }
            }
        }
        const auto before = snapshot();
        if (!failed && before.online) connection_since = now;
        if (!EspNowLink::is_pairing() && now - connection_since >= RuntimeSettings::get(RuntimeSettings::Id::ConnectMs) * 1000LL && !off_required) {
            finish_operation(on_operation, "ON", "cancelled_by_link_loss", now);
            begin_operation(off_operation, operation_serial, "OFF", "link_lost", now);
            off_logged = false;
            // 失联时重新尝试关断，使用独立且固定的关断期限；不可无限延长。
            off_required = true;
            off_since = now;
            pending = false; id = 0; on_after_off = false;
            EspNowLink::cancel_transmissions();
            current_data_request = 0;
            retry_at = 0;
        }
        const bool stop_expired = !EspNowLink::is_pairing() && off_required && now - off_since >= RuntimeSettings::get(RuntimeSettings::Id::ConnectMs) * 1000LL;
        if (!failed && stop_expired) {
            finish_operation(on_operation, "ON", "cancelled_by_OFF_timeout", now);
            finish_operation(off_operation, "OFF", "unconfirmed_timeout", now);
            failed = true;
            pending = false;
            id = 0;
            on_after_off = false;
            recovery_needed = false;
            EspNowLink::cancel_transmissions();
            current_data_request = 0;
            EspNowLink::leave_pairing_mode();
            (void)WiFiManager::instance().stop();
            portENTER_CRITICAL(&lock);
            model.stop_timed_out = stop_expired;
            portEXIT_CRITICAL(&lock);
            set_state(before.paired ? State::OFFLINE : State::UNPAIRED, "OFF_total_timeout");
            APP_LOGI(kEventTag, "retries stopped close_timeout=%u output_confirmed=%u output=%u",
                           stop_expired, before.output_confirmed, before.output_on);
        }
        if (!failed) {
            portENTER_CRITICAL(&lock);
            model.stop_timed_out = false;
            portEXIT_CRITICAL(&lock);
        }
        // 把本轮结果写回快照：配对/忙/连接失败等派生字段，以及睡前握手所需的 quiesced。
        EspNowLink::SavedPeer saved{};
        const bool paired = EspNowLink::get_saved_peer(0, &saved) == ESP_OK;
        portENTER_CRITICAL(&lock);
        model.stop_closed = low;
        model.paired = paired;
        model.pairing = EspNowLink::is_pairing();
        model.connection_failed = failed;
        model.busy = !failed && (pending || off_required || on_after_off || (release_armed && !low) || model.pairing ||
                                 EspNowLink::is_recovering_channel() || recovery_needed);
        // 暂停后仅在无待办且已确认 OFF，或本轮关断期限已耗尽时静止。
        // 超时允许休眠是产品策略，不能将其记录为已确认关闭。
        model.quiesced =
            pause_requested.load() && !model.busy &&
            (model.stop_timed_out || (!model.output_on && model.output_confirmed &&
             model.output_time_us > 0 && now - model.output_time_us < RuntimeSettings::get(RuntimeSettings::Id::FreshMs) * 1000LL)) &&
            !contact_fell.load() && !stop_requested.load() && !on_requested.load() && !pair_requested.load();
        if (paired)
            controller = saved.address;
        const bool quiesced = model.quiesced;
        const auto current_data = model;
        portEXIT_CRITICAL(&lock);
        if (current_data.output_on != output_before || current_data.output_confirmed != confirmed_before) {
            APP_LOGI(kEventTag, "output value=%u confirmed=%u state=%s evidence=%s",
                     current_data.output_on, current_data.output_confirmed,
                     !current_data.output_confirmed ? "unconfirmed" : current_data.output_on ? "ON" : "OFF",
                     current_data.output_time_us == 0 ? "unknown" : "ack_or_matched_telemetry");
            output_before = current_data.output_on; confirmed_before = current_data.output_confirmed;
        }
        if (current_data.protection_mask != protection_before) {
            const unsigned mask = current_data.protection_mask;
            APP_LOGI(kEventTag, "protection old=0x%x new=0x%x OTP=%u OVP=%u UVP=%u OCP=%u source=remote",
                     protection_before, mask, !!(mask&1), !!(mask&2), !!(mask&4), !!(mask&8));
            APP_LOGI(kEventTag, "protection sample valid=%u age_ms=%lld V_mv=%u I_ua=%ld board_cC=%d chip_cC=%d",
                     current_data.data_time_us > 0,
                     static_cast<long long>(current_data.data_time_us > 0 ? (now-current_data.data_time_us)/1000 : -1),
                     current_data.data.voltage_mv, static_cast<long>(current_data.data.current_ua),
                     current_data.data.board_temperature_centi_c, current_data.data.chip_temperature_centi_c);
            protection_before = current_data.protection_mask;
        }
        if (recovery_pending && !EspNowLink::is_recovering_channel()) {
            recovery_log.observe(TAG, "peer channel recovery", EspNowLink::get_channel_recovery_result());
            recovery_pending = false;
        }
        // 连接已失败或已静止时不再推进事务，仅维持心跳节拍。
        if (failed || quiesced) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!paired) {
            set_state(State::UNPAIRED);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (EspNowLink::is_pairing()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // 急停授权使用独立心跳；超时后功率计禁止任何来源开启。
        const bool inhibited = low || off_required || failed || stop_requested.load() || contact_fell.load();
        if (!EspNowLink::is_recovering_channel() && (now >= interlock_at || inhibited != interlock_before)) {
            const uint8_t payload = inhibited ? 1 : 0;
            EspNowLink::SendOptions options = {};
            const auto error = EspNowLink::send(saved.address, EspNowService::Internal::MSG_REMOTE_INTERLOCK,
                                                &payload, sizeof(payload), options);
            if (error == ESP_OK) {
                interlock_before = inhibited;
                interlock_at = now + 500000;
            } else {
                interlock_at = now + 100000;
            }
        }

        // 应答超时：清除在途事务并触发信道恢复；完成关断阶段使用更长的重试间隔。
        if (pending && now >= deadline) {
            const uint32_t timed_out_id = id;
            pending = false;
            id = 0;
            EspNowLink::cancel_transmissions();
            current_data_request = 0;
            if (action == EspNowService::SwitchAction::ON) {
                finish_operation(on_operation, "ON", "ack_timeout_output_unknown", now);
                begin_operation(off_operation, operation_serial, "OFF", "ON_ack_timeout", now);
                off_logged = false;
                // ON 执行结果不明，转为有期限的 OFF 同步，不继续重放 ON。
                off_required = true;
                off_since = now;
                on_after_off = false;
            }
            recovery_needed = true;
            retry_at = action == EspNowService::SwitchAction::ON ? now :
                now + RuntimeSettings::get(RuntimeSettings::Id::OffRetryMs) * 1000LL;
            set_state(State::OFFLINE, "ack_timeout");
            APP_LOGI(TAG, "switch timeout id=%lu action=%u OFF_pending=%u",
                     static_cast<unsigned long>(timed_out_id), static_cast<unsigned>(action), off_required);
        }
        // 急停保持期间，只依据控制应答或匹配当前请求的遥测更新后的输出状态再次关断。
        // 显示用 data.status_flags 可能仍缓存关断前的 ON，不能推翻已收到的 OFF 确认。
        if (low && current_data.output_on && current_data.output_time_us > 0 &&
            !pending && !off_required) {
            begin_operation(off_operation, operation_serial, "OFF", "output_ON_while_stop_held", now);
            off_logged = false;
            off_required = true;
            off_since = now;
            EspNowLink::cancel_transmissions();
            current_data_request = 0;
        }
        if (on_after_off && !off_required && now >= on_submit_until) {
            finish_operation(on_operation, "ON", "submit_timeout", now);
            on_after_off = false;
            set_state(State::OFFLINE, "ON_submit_timeout");
        }
        // 链路异常时按约 3 秒间隔发起一次对端信道恢复，直到重新可达。
        if (recovery_needed && !pending && now >= recover_at && !EspNowLink::is_recovering_channel()) {
            const auto err = EspNowLink::recover_peer_channel(saved.address);
            APP_LOGI(TAG, "CHANNEL_RECOVERY submit=%s", esp_err_to_name(err));
            recovery_submit_log.observe(TAG, "peer channel recovery submit", err);
            recovery_pending = err == ESP_OK;
            recover_at = now + 3000000;
            recovery_needed = false;
        }
        // 发送时机：无在途事务、未在恢复信道、到达重试时间，且存在 OFF 需求
        // 或待发 ON（此时触点已释放）。
        if (!pending && !EspNowLink::is_recovering_channel() && now >= retry_at &&
            (off_required || (on_after_off && !low && !Hardware::stop_closed() &&
             !contact_fell.load() && !stop_requested.load()))) {
            action = off_required ? EspNowService::SwitchAction::OFF : EspNowService::SwitchAction::ON;
            const auto err = EspNowService::send_switch_request(saved.address, action, &id);
            auto& operation = action == EspNowService::SwitchAction::ON ? on_operation : off_operation;
            ++operation.attempts;
            submit_log.observe(TAG, "switch submit", err);
            if (err == ESP_OK && (action == EspNowService::SwitchAction::ON || !off_logged)) {
                APP_LOGI(kEventTag, "switch TX op=%lu id=%lu action=%u", static_cast<unsigned long>(operation.number), static_cast<unsigned long>(id),
                               static_cast<unsigned>(action));
                if (action == EspNowService::SwitchAction::OFF)
                    off_logged = true;
            }
            APP_LOGI(TAG, "SWITCH_TX id=%lu action=%u submit=%s", static_cast<unsigned long>(id),
                     static_cast<unsigned>(action), esp_err_to_name(err));
            if (err == ESP_OK) {
                if (action == EspNowService::SwitchAction::ON &&
                    (Hardware::stop_closed() || contact_fell.load() || stop_requested.load())) {
                    // ESP32-C3 单核：本任务优先级高于链路，取消发生在链路出队前。
                    EspNowLink::cancel_transmissions();
                    continue;
                }
                portENTER_CRITICAL(&lock);
                if (action == EspNowService::SwitchAction::ON) {
                    release_resume = false;
                    // ON 已入发送队列，可能执行但尚未确认；保守锁存可能开启。
                    on_after_off = false;
                    maybe_on = true;
                    model.output_confirmed = false;
                    model.output_time_us = 0;
                    model.output_on = true;
                    ++model.on_attempt;
                }
                portEXIT_CRITICAL(&lock);
                set_state(off_required && !release_resume ? State::STOPPING : State::STARTING, "switch_submitted");
                pending = true;
                deadline = now + RuntimeSettings::get(off_required ? RuntimeSettings::Id::OffAckMs : RuntimeSettings::Id::OnAckMs) * 1000LL;
            } else {
                retry_at =
                    now + (off_required ? RuntimeSettings::get(RuntimeSettings::Id::OffRetryMs) * 1000LL : 100000);
                recovery_needed = true;
                set_state(State::OFFLINE, "switch_submit_failed");
            }
        }
        // 仅在无 OFF 待办时按 5Hz 轮询遥测；长时间收不到数据则触发信道恢复。
        if (!pending && !off_required && !EspNowLink::is_recovering_channel() && now >= data_at) {
            uint32_t requested_id = 0;
            const auto err = EspNowService::request_device_data(saved.address, &requested_id);
            if (err == ESP_OK) current_data_request = requested_id;
            data_at = now + 200000; // 5Hz 遥测轮询。
            data_log.observe(TAG, "data request", err);
            if (err != ESP_OK || (current_data.data_time_us > 0 && now - current_data.data_time_us > 3000000)) {
                recovery_needed = true;
            }
        }
        // 若 3 秒内的遥测仍带保护标志，则对外显示为受保护状态。
        if (!pending && !off_required && current_data.protection_mask && current_data.data_time_us > 0 &&
            now - current_data.data_time_us < 3000000)
            set_state(State::PROTECTED, "remote_protection");
        else if (!pending && !off_required && !on_after_off && current_data.state == State::PROTECTED &&
                 !current_data.protection_mask && current_data.data_time_us > 0 &&
                 current_data.data_time_us > protection_refused_at &&
                 now - current_data.data_time_us < 3000000)
            set_state(current_data.output_on ? State::ON : State::OFF, "remote_protection_cleared");
        vTaskDelay(pdMS_TO_TICKS(10)); // 主循环固定 10ms 节拍。
    }
}
} // namespace

// 初始化无线服务、注册各回调，并拉起独立工作线程（优先级 5，栈 5KB）。
esp_err_t init(bool resume_release) {
    resume_release_on_boot = resume_release;
    responses = xQueueCreate(16, sizeof(Response));
    telemetry = xQueueCreate(1, sizeof(Telemetry));
    if (!responses || !telemetry)
        return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(WiFiManager::instance().init());
    ESP_ERROR_CHECK(EspNowService::init());
    ESP_ERROR_CHECK(EspNowLink::configure_pairing(EspNowService::pairing_config(EspNowService::PairingRole::EMERGENCY_STOP)));
    EspNowService::set_switch_response_handler(receive_switch);
    EspNowService::set_data_received_handler(receive_data);
    ESP_ERROR_CHECK(EspNowLink::register_handler(kDetailMessage, receive_detail));
    // Link 初始化已恢复 NVS 配对；首次业务报文必须直接使用保存信道。
    // 固定从信道 1 开始会使其他信道上的节点先超时，再额外等待恢复与 OFF 重试。
    EspNowLink::SavedPeer saved{};
    const bool saved_channel = EspNowLink::get_saved_peer(0, &saved) == ESP_OK &&
                               saved.last_channel >= 1 && saved.last_channel <= 14;
    const uint8_t channel = saved_channel ? saved.last_channel : 1;
    ESP_ERROR_CHECK(WiFiManager::instance().start_sta_radio(channel));
    APP_LOGI(kEventTag, "startup radio channel=%u source=%s release_wake=%u",
             channel, saved_channel ? "saved_peer" : "default", resume_release);
    return xTaskCreate(worker, "emergency_remote", 5120, nullptr, 5, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
// ISR 锁存接口：仅置位标志，实际处理留给工作线程（relaxed 足够，消费者用 exchange 读取）。
void IRAM_ATTR stop_from_isr() {
    contact_fell.store(true, std::memory_order_relaxed);
    EspNowLink::cancel_transmissions_from_isr();
}
void request_stop() {
    stop_requested.store(true);
    EspNowLink::cancel_transmissions();
}
void request_on() { on_requested.store(true); }
// 启动配对：成功时由 Link 原子替换旧绑定，失败保留旧绑定。
void start_pairing() { pair_requested.store(true); }
// 删除配对：交由工作线程清空绑定并停在未配对状态。
void delete_pairing() { delete_requested.store(true); }
// 睡前握手：请求暂停并最多等待 100ms，直到快照报告 quiesced；超时则撤销暂停并返回失败。
bool prepare_sleep() {
    pause_requested.store(true);
    for (int i = 0; i < 10; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (snapshot().quiesced)
            return true;
    }
    pause_requested.store(false);
    return false;
}
void cancel_sleep() { pause_requested.store(false); }
void retry_connection() { retry_requested.store(true); }
// 仅当在线、连接未失败且存在已配对节点时才上报电量。
void report_battery(uint8_t percent) {
    static AppDiagnostics::ErrorLog report_log; // 唯一调用者为协调任务。
    EspNowLink::SavedPeer peer{};
    const auto s = snapshot();
    if (!s.online || s.connection_failed || EspNowLink::get_saved_peer(0, &peer) != ESP_OK)
        return;
    const auto err = EspNowService::send_remote_battery(peer.address, percent);
    report_log.observe(TAG, "battery report", err);
    APP_LOGI(TAG, "battery report percent=%u result=%s", percent, esp_err_to_name(err));
}
void test_wrong_channel() { wrong_channel_requested.store(true); }
// 复制快照并补全派生字段：并入原子请求的忙位、校验静止一致性、按新鲜度判定在线。
Snapshot snapshot() {
    portENTER_CRITICAL(&lock);
    Snapshot copy = model;
    portEXIT_CRITICAL(&lock);
    // 任何尚未被工作线程消费的原子请求都视为忙，避免 UI 误判为空闲。
    copy.busy = copy.busy || contact_fell.load() || stop_requested.load() || on_requested.load() ||
                pair_requested.load() || delete_requested.load() || retry_requested.load();
    // 静止还必须满足触点读取未发生变化，否则说明存在竞态。
    copy.quiesced = copy.quiesced && !copy.busy && copy.stop_closed == (Hardware::stop_closed());
    copy.online =
        copy.data_time_us > 0 && esp_timer_get_time() - copy.data_time_us < RuntimeSettings::get(RuntimeSettings::Id::FreshMs) * 1000LL;
    return copy;
}
} // namespace EmergencyRemote
