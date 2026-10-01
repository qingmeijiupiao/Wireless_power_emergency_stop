/**
 * @file emergency_remote.cpp
 * @brief 急停遥控从机实现：独立工作线程、急停 ISR 锁存、ON/OFF 事务与 ACK 超时重试、
 *        信道恢复、配对状态机、数据快照与睡前握手。
 */
#include "emergency_remote.h"
#include "runtime_settings.h"
#include "diagnostic_log.h"
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
std::atomic_bool wrong_channel_requested{false}, repair_requested{false}, pause_requested{false};
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
// 非阻塞入队：队列满时丢弃应答并记录错误，使 OFF 事务保持待处理以便重试。
void enqueue(const Response &rsp) {
    if (xQueueSend(responses, &rsp, 0) != pdTRUE)
        ESP_LOGE(TAG, "response queue full; OFF remains pending");
}

// 鉴权：仅当已配对且来源 MAC 等于已保存控制端时才接受报文，避免伪造或串扰。
bool accept_peer(const EspNowLink::MacAddress &source) {
    portENTER_CRITICAL(&lock);
    const bool accept = model.paired && source == controller;
    portEXIT_CRITICAL(&lock);
    return accept;
}

// 精简开关应答回调：鉴权通过后打包成 Response 交给工作线程处理。
void receive_switch(const EspNowLink::MacAddress &source, uint32_t id, EspNowService::SwitchAction action,
                    EspNowService::SwitchResult result, bool output, void *) {
    if (!accept_peer(source))
        return;
    Response rsp{};
    rsp.id = id;
    rsp.action = action;
    rsp.result = result;
    rsp.output = output;
    enqueue(rsp);
}

// 详细开关应答回调：校验可靠性、长度与各字段取值域后解析成 Response。
void receive_detail(const EspNowLink::Message &message, void *) {
    if (!message.reliable || message.payload_size != 9 || !accept_peer(message.source) || message.payload[4] > 2 ||
        message.payload[5] > 4 || message.payload[6] > 1 || message.payload[7] > 10 || message.payload[8] > 15)
        return;
    const uint32_t id = EspNowLink::Codec::load_le<uint32_t>(message.payload);
    if (!id)
        return;
    Response rsp{};
    rsp.id = id;
    rsp.action = static_cast<EspNowService::SwitchAction>(message.payload[4]);
    rsp.result = static_cast<EspNowService::SwitchResult>(message.payload[5]);
    rsp.output = message.payload[6] != 0;
    rsp.reason = message.payload[7];
    rsp.mask = message.payload[8];
    enqueue(rsp);
}

// 遥测数据回调：刷新数据快照、输出状态与保护位；时间戳用于在线与新鲜度判定。
void receive_data(const EspNowLink::MacAddress &source, uint32_t, const EspNowService::DeviceData &data, bool available,
                  bool, void *) {
    if (!available || !accept_peer(source))
        return;
    portENTER_CRITICAL(&lock);
    model.data = data;
    model.data_time_us = esp_timer_get_time();
    model.output_on = (data.status_flags & 1) != 0;
    model.output_time_us = model.data_time_us;
    model.protection_mask = (data.status_flags >> 1) & 15;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "DATA voltage_mv=%u current_ua=%ld output=%u flags=%u", data.voltage_mv,
             static_cast<long>(data.current_ua), (data.status_flags & 1) != 0, data.status_flags);
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

// 在临界区内更新对外状态。
void set_state(State state) {
    portENTER_CRITICAL(&lock);
    model.state = state;
    portEXIT_CRITICAL(&lock);
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
    bool off_required = true; // 重启/配对后始终先同步 OFF，启动时绝不自动开输出。
    bool pending = false, recovery_needed = false;
    uint32_t id = 0;
    auto action = EspNowService::SwitchAction::OFF;
    // deadline=应答超时；retry_at=下次可发送；high_since=触点释放起点；
    // data_at=下次遥测轮询；recover_at=下次信道恢复。
    int64_t deadline = 0, retry_at = 0, high_since = 0, data_at = 0, recover_at = 0;
    int64_t connection_since = esp_timer_get_time();
    bool failed = false;     // 连接重试是否已耗尽
    bool off_logged = false; // 同一轮 OFF 业务事件只记录一次，避免刷屏
    EspNowLink::LinkStatistics last_stats{};
    int64_t stats_at = 0;
    while (true) {
        const int64_t now = esp_timer_get_time();
        const bool low = Hardware::stop_closed();
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
                ESP_LOGW(TAG,
                         "ESPNOW delta submit=%lu mac=%lu ack_timeout=%lu rx_invalid=%lu rx_overflow=%lu",
                         static_cast<unsigned long>(submit), static_cast<unsigned long>(mac),
                         static_cast<unsigned long>(ack), static_cast<unsigned long>(invalid),
                         static_cast<unsigned long>(overflow));
            last_stats = stats;
            stats_at = now + 1000000;
        }
        if (wrong_channel_requested.exchange(false)) {
            ESP_LOGW(TAG, "TEST_WRONG_CHANNEL submit=%s", esp_err_to_name(WiFiManager::instance().set_channel(6)));
        }
        // 消费本拍到来的急停/停止/重试请求；exchange 保证每个事件只被处理一次。
        const bool fall = contact_fell.exchange(false);
        const bool stop = stop_requested.exchange(false);
        if (retry_requested.exchange(false)) {
            invalidate_sleep();
            failed = false;
            connection_since = now;
            retry_at = recover_at = 0;
            pending = false;
            id = 0;
            recovery_needed = true;
            off_required = true;
            on_after_off = false;
            ESP_LOGI(TAG, "USER_RETRY window_ms=%lu", static_cast<unsigned long>(RuntimeSettings::get("connect_ms")));
            DEVICE_EVENT_I(kEventTag, "connection retry requested");
            off_logged = false;
        }
        // 急停锁存：无论是 ISR 边沿、任务请求还是轮询首次发现的闭合，都强制进入 OFF
        // 事务并作废睡眠，同时清除尚未发送的 ON。
        if (stop || fall || (low && !was_low)) {
            invalidate_sleep();
            failed = false;
            connection_since = now;
            off_required = true;
            on_after_off = false;
            release_armed = low || fall;
            high_since = low ? 0 : now;
            pending = false;
            id = 0;
            retry_at = 0;
            set_state(State::STOPPING);
            DEVICE_EVENT_I(kEventTag, "STOP latched contact=%d edge=%u requested=%u", low, fall, stop);
            off_logged = false;
        }
        // 触点仍闭合则持续压制释放计时；只有高电平保持超过 release_ms 才认定释放稳定。
        if (low)
            high_since = 0;
        else {
            if (!high_since)
                high_since = now;
            if (release_armed && now - high_since >= RuntimeSettings::get("release_ms") * 1000LL) {
                release_armed = false;
                on_after_off = true;
                ESP_LOGI(TAG, "RELEASE_STABLE; ON waits for confirmed OFF");
                DEVICE_EVENT_I(kEventTag, "STOP released stable; ON queued after OFF confirmation");
            }
        }
        was_low = low;
        // 诊断用 ON 请求：触点闭合或释放尚未稳定时一律忽略，防止越过安全约束。
        if (on_requested.exchange(false) && !low && !release_armed) {
            invalidate_sleep();
            failed = false;
            connection_since = now;
            on_after_off = true;
            DEVICE_EVENT_I(kEventTag, "ON requested");
        }
        // 链路未激活（例如掉线）时，按已保存节点的信道重新拉起 STA 无线。
        if (!failed && !EspNowLink::is_active()) {
            EspNowLink::SavedPeer peer{};
            const uint8_t channel = EspNowLink::get_saved_peer(0, &peer) == ESP_OK ? peer.last_channel : 1;
            const auto err = WiFiManager::instance().start_sta_radio(channel);
            if (err != ESP_OK)
                ESP_LOGE(TAG, "radio resume channel=%u: %s", channel, esp_err_to_name(err));
            else
                DEVICE_EVENT_I(kEventTag, "radio resumed channel=%u", channel);
        }
        // 配对请求：仅当无待办事务、未排队 ON、未在恢复信道/配对中，且当前输出安全时才受理。
        if (pair_requested.exchange(false)) {
            const auto status = snapshot();
            invalidate_sleep();
            if (!pending && !on_after_off && !EspNowLink::is_recovering_channel() && !EspNowLink::is_pairing() &&
                (!status.paired || (!off_required && !status.output_on && status.output_time_us > 0 &&
                                    now - status.output_time_us < RuntimeSettings::get("fresh_ms") * 1000LL))) {
                // 重新配对：先清空已保存节点及输出/数据时间戳，强制重新同步 OFF。
                if (repair_requested.exchange(false)) {
                    DEVICE_EVENT_I(kEventTag, "pairing: clear saved peers");
                    EspNowLink::SavedPeer old{};
                    while (EspNowLink::get_saved_peer(0, &old) == ESP_OK) {
                        EspNowLink::remove_peer(old.address);
                        if (EspNowLink::remove_saved_peer(old.address) != ESP_OK)
                            break;
                    }
                    portENTER_CRITICAL(&lock);
                    model.output_time_us = 0;
                    model.data_time_us = 0;
                    portEXIT_CRITICAL(&lock);
                    off_required = true;
                }
                failed = false;
                connection_since = now;
                if (!EspNowLink::is_active())
                    WiFiManager::instance().start_sta_radio(1);
                const auto err = EspNowLink::start_pairing();
                DEVICE_EVENT_I(kEventTag, "pairing start: %s", esp_err_to_name(err));
            } else
                DEVICE_EVENT_I(kEventTag, "pairing denied: unsafe output or pending transaction");
        }
        // 在线时刷新连接计时；超过 connect_ms 仍未连通且不需要完成关断，则判定连接失败并关无线。
        const auto before = snapshot();
        if (!failed && before.online)
            connection_since = now;
        const bool exhausted = now - connection_since >= RuntimeSettings::get("connect_ms") * 1000LL;
        const bool must_finish_stop = before.output_on && off_required;
        if (!failed && exhausted && !must_finish_stop) {
            failed = true;
            pending = false;
            id = 0;
            on_after_off = false;
            recovery_needed = false;
            EspNowLink::leave_pairing_mode();
            ESP_LOGI(TAG, "RADIO_STOP %s", esp_err_to_name(WiFiManager::instance().stop()));
            set_state(before.paired ? State::OFFLINE : State::UNPAIRED);
            ESP_LOGW(TAG, "CONNECTION_FAILED retries stopped; BOOT short press retries");
            DEVICE_EVENT_I(kEventTag, "connection failed; retries stopped output=%u", before.output_on);
        }
        // 把本轮结果写回快照：配对/忙/连接失败等派生字段，以及睡前握手所需的 quiesced。
        EspNowLink::SavedPeer saved{};
        const bool paired = EspNowLink::get_saved_peer(0, &saved) == ESP_OK;
        portENTER_CRITICAL(&lock);
        model.stop_closed = low;
        model.paired = paired;
        model.pairing = EspNowLink::is_pairing();
        model.connection_failed = failed || (exhausted && must_finish_stop);
        model.busy = !failed && (pending || off_required || on_after_off || (release_armed && !low) || model.pairing ||
                                 EspNowLink::is_recovering_channel() || recovery_needed);
        // quiesced 仅在已请求暂停、无待办、输出确已断开且有新鲜状态证据，
        // 且没有新的原子请求到来时成立。
        model.quiesced =
            pause_requested.load() && !model.busy && !model.output_on &&
            ((failed && !model.output_on) ||
             (model.output_time_us > 0 && now - model.output_time_us < RuntimeSettings::get("fresh_ms") * 1000LL)) &&
            !contact_fell.load() && !stop_requested.load() && !on_requested.load() && !pair_requested.load();
        if (paired)
            controller = saved.address;
        const bool quiesced = model.quiesced;
        const auto current_data = model;
        portEXIT_CRITICAL(&lock);
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

        // 排空应答队列，只采纳与当前在途事务 id/动作匹配的应答。
        Response rsp{};
        while (xQueueReceive(responses, &rsp, 0) == pdTRUE) {
            if (rsp.id == id && rsp.action == action) {
                connection_since = now; // 收到有效业务应答即证明对端可达。
                const bool first_response = pending;
                pending = false;
                portENTER_CRITICAL(&lock);
                model.output_on = rsp.output;
                model.output_time_us = now;
                // 精简应答不会覆盖由详细应答建立的更新的保护原因。
                if (rsp.reason != 10 || rsp.result == EspNowService::SwitchResult::OK)
                    model.protection_mask = rsp.mask;
                portEXIT_CRITICAL(&lock);
                // 成功必须同时满足“结果 OK”且“输出状态与请求动作一致”。
                const bool success = rsp.result == EspNowService::SwitchResult::OK &&
                                     rsp.output == (action == EspNowService::SwitchAction::ON);
                if (success && first_response)
                    DEVICE_EVENT_I(kEventTag, "switch ACK id=%lu action=%u output=%u", static_cast<unsigned long>(id),
                                   static_cast<unsigned>(action), rsp.output);
                else if (!success)
                    ESP_LOGE(TAG,
                             "switch refused id=%lu action=%u result=%u output=%u reason=%u protect=%u",
                             static_cast<unsigned long>(id), static_cast<unsigned>(action),
                             static_cast<unsigned>(rsp.result), rsp.output, rsp.reason, rsp.mask);
                ESP_LOGI(TAG, "SWITCH_ACK id=%lu action=%u result=%u output=%u reason=%u protect=%u",
                         static_cast<unsigned long>(id), static_cast<unsigned>(action),
                         static_cast<unsigned>(rsp.result), rsp.output, rsp.reason, rsp.mask);
                // OFF 失败需继续重试并保持 off_required；ON 仅在不需要关断时才切换状态。
                if (action == EspNowService::SwitchAction::OFF) {
                    off_required = !success;
                    retry_at = success ? 0 : now + 100000;
                    set_state(success ? State::OFF : State::OFFLINE);
                } else if (!off_required) {
                    if (rsp.reason != 10 || success || first_response)
                        set_state(success ? State::ON : refusal_state(rsp));
                }
            }
        }
        // 应答超时：清除在途事务并触发信道恢复；完成关断阶段使用更长的重试间隔。
        if (pending && now >= deadline) {
            pending = false;
            recovery_needed = true;
            retry_at = now + (exhausted && must_finish_stop ? RuntimeSettings::get("off_retry_ms") * 1000LL : 100000);
            set_state(State::OFFLINE);
            ESP_LOGW(TAG, "switch timeout id=%lu action=%u OFF_pending=%u",
                     static_cast<unsigned long>(id), static_cast<unsigned>(action), off_required);
        }
        // 急停保持期间，若已配对计量端被外部开输出，必须再次将其关断。
        if (low && current_data.output_on && current_data.data_time_us > 0 && !pending)
            off_required = true;
        // 链路异常时按约 3 秒间隔发起一次对端信道恢复，直到重新可达。
        if (recovery_needed && !pending && now >= recover_at && !EspNowLink::is_recovering_channel()) {
            const auto err = EspNowLink::recover_peer_channel(saved.address);
            ESP_LOGI(TAG, "CHANNEL_RECOVERY submit=%s", esp_err_to_name(err));
            ESP_LOGW(TAG, "channel recovery: %s", esp_err_to_name(err));
            recover_at = now + 3000000;
            recovery_needed = false;
        }
        // 发送时机：无在途事务、未在恢复信道、到达重试时间，且存在 OFF 需求
        // 或待发 ON（此时触点已释放）。
        if (!pending && !EspNowLink::is_recovering_channel() && now >= retry_at &&
            (off_required || (on_after_off && !low))) {
            action = off_required ? EspNowService::SwitchAction::OFF : EspNowService::SwitchAction::ON;
            if (action == EspNowService::SwitchAction::ON)
                on_after_off = false;
            const auto err = EspNowService::send_switch_request(saved.address, action, &id);
            if (err != ESP_OK)
                ESP_LOGE(TAG, "switch submit action=%u: %s",
                         static_cast<unsigned>(action), esp_err_to_name(err));
            else if (action == EspNowService::SwitchAction::ON || !off_logged) {
                DEVICE_EVENT_I(kEventTag, "switch TX id=%lu action=%u", static_cast<unsigned long>(id),
                               static_cast<unsigned>(action));
                if (action == EspNowService::SwitchAction::OFF)
                    off_logged = true;
            }
            ESP_LOGI(TAG, "SWITCH_TX id=%lu action=%u submit=%s", static_cast<unsigned long>(id),
                     static_cast<unsigned>(action), esp_err_to_name(err));
            if (err == ESP_OK) {
                portENTER_CRITICAL(&lock);
                if (action == EspNowService::SwitchAction::ON) {
                    // 业务应答尚未证明成功前，ON 可能已到达计量端，故先乐观置位输出。
                    model.output_on = true;
                    ++model.on_attempt;
                }
                model.state = off_required ? State::STOPPING : State::STARTING;
                portEXIT_CRITICAL(&lock);
                pending = true;
                deadline = now + RuntimeSettings::get(off_required ? "off_ack_ms" : "on_ack_ms") * 1000LL;
            } else {
                retry_at =
                    now + (exhausted && must_finish_stop ? RuntimeSettings::get("off_retry_ms") * 1000LL : 100000);
                recovery_needed = true;
                set_state(State::OFFLINE);
            }
        }
        // 仅在无 OFF 待办时按 5Hz 轮询遥测；长时间收不到数据则触发信道恢复。
        if (!pending && !off_required && !EspNowLink::is_recovering_channel() && now >= data_at) {
            const auto err = EspNowService::request_device_data(saved.address);
            data_at = now + 200000; // 5Hz 遥测轮询。
            if (err != ESP_OK)
                ESP_LOGE(TAG, "data request: %s", esp_err_to_name(err));
            if (err != ESP_OK || (current_data.data_time_us > 0 && now - current_data.data_time_us > 3000000)) {
                recovery_needed = true;
            }
        }
        // 若 3 秒内的遥测仍带保护标志，则对外显示为受保护状态。
        if (!pending && !off_required && current_data.protection_mask && current_data.data_time_us > 0 &&
            now - current_data.data_time_us < 3000000)
            set_state(State::PROTECTED);
        vTaskDelay(pdMS_TO_TICKS(10)); // 主循环固定 10ms 节拍。
    }
}
} // namespace

// 初始化无线服务、注册各回调，并拉起独立工作线程（优先级 5，栈 5KB）。
esp_err_t init(bool resume_release) {
    resume_release_on_boot = resume_release;
    responses = xQueueCreate(16, sizeof(Response));
    if (!responses)
        return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(WiFiManager::instance().init());
    ESP_ERROR_CHECK(EspNowService::init());
    EspNowService::set_switch_response_handler(receive_switch);
    EspNowService::set_data_received_handler(receive_data);
    ESP_ERROR_CHECK(EspNowLink::register_handler(kDetailMessage, receive_detail));
    ESP_ERROR_CHECK(WiFiManager::instance().start_sta_radio(1));
    return xTaskCreate(worker, "emergency_remote", 5120, nullptr, 5, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
// ISR 锁存接口：仅置位标志，实际处理留给工作线程（relaxed 足够，消费者用 exchange 读取）。
void IRAM_ATTR stop_from_isr() { contact_fell.store(true, std::memory_order_relaxed); }
void request_stop() { stop_requested.store(true); }
void request_on() { on_requested.store(true); }
// 启动配对：clear_first 同时置位 repair 标志，由工作线程先清除旧节点。
void start_pairing(bool clear_first) {
    repair_requested.store(clear_first);
    pair_requested.store(true);
}
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
    EspNowLink::SavedPeer peer{};
    const auto s = snapshot();
    if (!s.online || s.connection_failed || EspNowLink::get_saved_peer(0, &peer) != ESP_OK)
        return;
    const auto err = EspNowService::send_remote_battery(peer.address, percent);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "battery report percent=%u: %s", percent, esp_err_to_name(err));
}
void test_wrong_channel() { wrong_channel_requested.store(true); }
// 复制快照并补全派生字段：并入原子请求的忙位、校验静止一致性、按新鲜度判定在线。
Snapshot snapshot() {
    portENTER_CRITICAL(&lock);
    Snapshot copy = model;
    portEXIT_CRITICAL(&lock);
    // 任何尚未被工作线程消费的原子请求都视为忙，避免 UI 误判为空闲。
    copy.busy = copy.busy || contact_fell.load() || stop_requested.load() || on_requested.load() ||
                pair_requested.load() || retry_requested.load();
    // 静止还必须满足触点读取未发生变化，否则说明存在竞态。
    copy.quiesced = copy.quiesced && !copy.busy && copy.stop_closed == (Hardware::stop_closed());
    copy.online =
        copy.data_time_us > 0 && esp_timer_get_time() - copy.data_time_us < RuntimeSettings::get("fresh_ms") * 1000LL;
    return copy;
}
} // namespace EmergencyRemote
