#include "emergency_remote.h"
#include "HXC_NVS.h"
#include "runtime_settings.h"
#include "wifi_manager.h"
#include "espnow_codec.h"
#include "driver/gpio.h"
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
constexpr uint16_t kDetailMessage = 0x0203;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
Snapshot model{};
EspNowLink::MacAddress controller{};
std::atomic_bool contact_fell{false}, stop_requested{false}, on_requested{false}, pair_requested{false};
std::atomic_bool wrong_channel_requested{false}, repair_requested{false}, pause_requested{false};
std::atomic_bool retry_requested{false};
bool resume_release_on_boot = false;
struct Response {
    uint32_t id = 0, serial = 0;
    EspNowService::SwitchAction action{};
    EspNowService::SwitchResult result{};
    bool output = false;
    uint8_t reason = 10, mask = 0;
};
QueueHandle_t responses = nullptr;
void enqueue(const Response& rsp) {
    if (xQueueSend(responses, &rsp, 0) != pdTRUE)
        ESP_LOGW(TAG, "Response queue full; unconfirmed OFF will retry");
}

bool accept_peer(const EspNowLink::MacAddress& source) {
    portENTER_CRITICAL(&lock);
    const bool accept = model.paired && source == controller;
    portEXIT_CRITICAL(&lock);
    return accept;
}

void receive_switch(const EspNowLink::MacAddress& source, uint32_t id,
                    EspNowService::SwitchAction action, EspNowService::SwitchResult result,
                    bool output, void*) {
    if (!accept_peer(source)) return;
    Response rsp{};
    rsp.id = id; rsp.action = action; rsp.result = result; rsp.output = output;
    enqueue(rsp);
}

void receive_detail(const EspNowLink::Message& message, void*) {
    if (!message.reliable || message.payload_size != 9 || !accept_peer(message.source) ||
        message.payload[4] > 2 || message.payload[5] > 4 || message.payload[6] > 1 ||
        message.payload[7] > 10 || message.payload[8] > 15) return;
    const uint32_t id = EspNowLink::Codec::load_le<uint32_t>(message.payload);
    if (!id) return;
    Response rsp{};
    rsp.id = id;
    rsp.action = static_cast<EspNowService::SwitchAction>(message.payload[4]);
    rsp.result = static_cast<EspNowService::SwitchResult>(message.payload[5]);
    rsp.output = message.payload[6] != 0;
    rsp.reason = message.payload[7]; rsp.mask = message.payload[8];
    enqueue(rsp);
}

void receive_data(const EspNowLink::MacAddress& source, uint32_t,
                  const EspNowService::DeviceData& data, bool available, bool, void*) {
    if (!available || !accept_peer(source)) return;
    portENTER_CRITICAL(&lock);
    model.data = data; model.data_time_us = esp_timer_get_time();
    model.output_on = (data.status_flags & 1) != 0;
    model.output_time_us = model.data_time_us;
    model.protection_mask = (data.status_flags >> 1) & 15;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "DATA voltage_mv=%u current_ua=%ld output=%u flags=%u",
        data.voltage_mv, static_cast<long>(data.current_ua), (data.status_flags & 1) != 0, data.status_flags);
}

State refusal_state(const Response& rsp) {
    switch (rsp.reason) {
    case 1: return State::SHORT_FAULT;
    case 2: return State::PROTECTED;
    case 3: return State::COOLDOWN;
    case 4: return State::BUSY;
    case 6: return State::NOT_READY;
    case 7: return State::DETECT_ERROR;
    default: return rsp.result == EspNowService::SwitchResult::NOT_READY ?
                          State::NOT_READY : State::REJECTED;
    }
}

void set_state(State state) {
    portENTER_CRITICAL(&lock); model.state = state; portEXIT_CRITICAL(&lock);
}

void invalidate_sleep() {
    portENTER_CRITICAL(&lock);
    model.busy = true; model.quiesced = false;
    portEXIT_CRITICAL(&lock);
}
void worker(void*) {
    bool was_low = gpio_get_level(GPIO_NUM_5) == 0;
    bool release_armed = was_low || resume_release_on_boot, on_after_off = false;
    bool off_required = true; // Reboot/pairing always synchronizes OFF; never auto-ON at startup.
    bool pending = false, recovery_needed = false;
    uint32_t id = 0;
    auto action = EspNowService::SwitchAction::OFF;
    int64_t deadline = 0, retry_at = 0, high_since = 0, data_at = 0, recover_at = 0;
    int64_t connection_since = esp_timer_get_time();
    bool failed = false;
    while (true) {
        const int64_t now = esp_timer_get_time();
        const bool low = gpio_get_level(GPIO_NUM_5) == 0;
        if (wrong_channel_requested.exchange(false)) {
            ESP_LOGW(TAG, "TEST_WRONG_CHANNEL submit=%s", esp_err_to_name(WiFiManager::instance().set_channel(6)));
        }
        const bool fall = contact_fell.exchange(false);
        const bool stop = stop_requested.exchange(false);
        if (retry_requested.exchange(false)) {
            invalidate_sleep();
            failed = false; connection_since = now; retry_at = recover_at = 0;
            pending = false; id = 0; recovery_needed = true; off_required = true;
            on_after_off = false;
            ESP_LOGI(TAG, "USER_RETRY window_ms=%lu", static_cast<unsigned long>(RuntimeSettings::get("connect_ms")));
        }
        if (stop || fall || (low && !was_low)) {
            invalidate_sleep();
            failed = false; connection_since = now;
            off_required = true; on_after_off = false;
            release_armed = low || fall; high_since = low ? 0 : now;
            pending = false; id = 0; retry_at = 0;
            set_state(State::STOPPING);
            ESP_LOGW(TAG, "STOP_LATCHED gpio5=%d", low ? 0 : 1);
        }
        if (low) high_since = 0;
        else {
            if (!high_since) high_since = now;
            if (release_armed && now - high_since >= RuntimeSettings::get("release_ms") * 1000LL) {
                release_armed = false; on_after_off = true;
                ESP_LOGI(TAG, "RELEASE_STABLE; ON waits for confirmed OFF");
            }
        }
        was_low = low;
        if (on_requested.exchange(false) && !low && !release_armed) {
            invalidate_sleep();
            failed = false; connection_since = now; on_after_off = true;
        }
        if (!failed && !EspNowLink::is_active()) {
            EspNowLink::SavedPeer peer{};
            const uint8_t channel = EspNowLink::get_saved_peer(0, &peer) == ESP_OK ? peer.last_channel : 1;
            ESP_LOGI(TAG, "RADIO_RESUME %s", esp_err_to_name(WiFiManager::instance().start_sta_radio(channel)));
        }
        if (pair_requested.exchange(false)) {
            const auto status = snapshot();
            invalidate_sleep();
            if (!pending && !on_after_off && !EspNowLink::is_recovering_channel() &&
                !EspNowLink::is_pairing() && (!status.paired ||
                (!off_required && !status.output_on && status.output_time_us > 0 &&
                 now - status.output_time_us < RuntimeSettings::get("fresh_ms") * 1000LL))) {
                if (repair_requested.exchange(false)) {
                    EspNowLink::SavedPeer old{};
                    while (EspNowLink::get_saved_peer(0, &old) == ESP_OK) {
                        EspNowLink::remove_peer(old.address);
                        if (EspNowLink::remove_saved_peer(old.address) != ESP_OK) break;
                    }
                    portENTER_CRITICAL(&lock);
                    model.output_time_us = 0; model.data_time_us = 0;
                    portEXIT_CRITICAL(&lock);
                    off_required = true;
                }
                failed = false; connection_since = now;
                if (!EspNowLink::is_active()) WiFiManager::instance().start_sta_radio(1);
                ESP_LOGI(TAG, "PAIR submit=%s", esp_err_to_name(EspNowLink::start_pairing()));
            } else ESP_LOGW(TAG, "PAIR_DENIED unsafe output or pending transaction");
        }
        const auto before = snapshot();
        if (!failed && before.online) connection_since = now;
        const bool exhausted = now - connection_since >= RuntimeSettings::get("connect_ms") * 1000LL;
        const bool must_finish_stop = before.output_on && off_required;
        if (!failed && exhausted && !must_finish_stop) {
            failed = true; pending = false; id = 0; on_after_off = false; recovery_needed = false;
            EspNowLink::leave_pairing_mode();
            ESP_LOGI(TAG, "RADIO_STOP %s", esp_err_to_name(WiFiManager::instance().stop()));
            set_state(before.paired ? State::OFFLINE : State::UNPAIRED);
            ESP_LOGW(TAG, "CONNECTION_FAILED retries stopped; BOOT short press retries");
        }
        EspNowLink::SavedPeer saved{};
        const bool paired = EspNowLink::get_saved_peer(0, &saved) == ESP_OK;
        portENTER_CRITICAL(&lock);
        model.stop_closed = low; model.paired = paired;
        model.pairing = EspNowLink::is_pairing();
        model.connection_failed = failed || (exhausted && must_finish_stop);
        model.busy = !failed && (pending || off_required || on_after_off || (release_armed && !low) ||
                     model.pairing || EspNowLink::is_recovering_channel() || recovery_needed);
        model.quiesced = pause_requested.load() && !model.busy && !model.output_on &&
            ((failed && !model.output_on) || (model.output_time_us > 0 &&
             now - model.output_time_us < RuntimeSettings::get("fresh_ms") * 1000LL)) &&
            !contact_fell.load() && !stop_requested.load() && !on_requested.load() && !pair_requested.load();
        if (paired) controller = saved.address;
        const bool quiesced = model.quiesced;
        const auto current_data = model;
        portEXIT_CRITICAL(&lock);
        if (failed || quiesced) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (!paired) { set_state(State::UNPAIRED); vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        Response rsp{};
        while (xQueueReceive(responses, &rsp, 0) == pdTRUE) {
            if (rsp.id == id && rsp.action == action) {
                connection_since = now; // A valid business reply proves the peer is reachable.
                const bool first_response = pending;
                pending = false;
                portENTER_CRITICAL(&lock);
                model.output_on = rsp.output;
                model.output_time_us = now;
                // Legacy responses do not clear a newer detailed protection reason.
                if (rsp.reason != 10 || rsp.result == EspNowService::SwitchResult::OK)
                    model.protection_mask = rsp.mask;
                portEXIT_CRITICAL(&lock);
                const bool success = rsp.result == EspNowService::SwitchResult::OK &&
                    rsp.output == (action == EspNowService::SwitchAction::ON);
                ESP_LOGI(TAG, "SWITCH_ACK id=%lu action=%u result=%u output=%u reason=%u protect=%u",
                    static_cast<unsigned long>(id), static_cast<unsigned>(action),
                    static_cast<unsigned>(rsp.result), rsp.output, rsp.reason, rsp.mask);
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
        if (pending && now >= deadline) {
            pending = false; recovery_needed = true;
            retry_at = now + (exhausted && must_finish_stop ? RuntimeSettings::get("off_retry_ms") * 1000LL : 100000);
            set_state(State::OFFLINE);
            ESP_LOGW(TAG, "SWITCH_TIMEOUT id=%lu action=%u; OFF remains latched=%u",
                static_cast<unsigned long>(id), static_cast<unsigned>(action), off_required);
        }
        // A paired meter that is externally turned ON while STOP is held must be stopped again.
        if (low && current_data.output_on && current_data.data_time_us > 0 && !pending) off_required = true;
        if (recovery_needed && !pending && now >= recover_at && !EspNowLink::is_recovering_channel()) {
            const auto err = EspNowLink::recover_peer_channel(saved.address);
            ESP_LOGI(TAG, "CHANNEL_RECOVERY submit=%s", esp_err_to_name(err));
            recover_at = now + 3000000; recovery_needed = false;
        }
        if (!pending && !EspNowLink::is_recovering_channel() && now >= retry_at &&
            (off_required || (on_after_off && !low))) {
            action = off_required ? EspNowService::SwitchAction::OFF : EspNowService::SwitchAction::ON;
            if (action == EspNowService::SwitchAction::ON) on_after_off = false;
            const auto err = EspNowService::send_switch_request(saved.address, action, &id);
            ESP_LOGI(TAG, "SWITCH_TX id=%lu action=%u submit=%s", static_cast<unsigned long>(id),
                     static_cast<unsigned>(action), esp_err_to_name(err));
            if (err == ESP_OK) {
                portENTER_CRITICAL(&lock);
                if (action == EspNowService::SwitchAction::ON) {
                    // Until a business response proves otherwise, ON may have reached the meter.
                    model.output_on = true;
                    ++model.on_attempt;
                }
                model.state = off_required ? State::STOPPING : State::STARTING;
                portEXIT_CRITICAL(&lock);
                pending = true; deadline = now + RuntimeSettings::get(off_required ? "off_ack_ms" : "on_ack_ms") * 1000LL;
            } else {
                retry_at = now + (exhausted && must_finish_stop ? RuntimeSettings::get("off_retry_ms") * 1000LL : 100000);
                recovery_needed = true; set_state(State::OFFLINE);
            }
        }
        if (!pending && !off_required && !EspNowLink::is_recovering_channel() && now >= data_at) {
            const auto err = EspNowService::request_device_data(saved.address);
            data_at = now + 1000000;
            if (err != ESP_OK || (current_data.data_time_us > 0 && now - current_data.data_time_us > 3000000)) {
                recovery_needed = true;
            }
        }
        if (!pending && !off_required && current_data.protection_mask &&
            current_data.data_time_us > 0 && now - current_data.data_time_us < 3000000) set_state(State::PROTECTED);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
}

esp_err_t init(bool resume_release) {
    resume_release_on_boot = resume_release;
    responses = xQueueCreate(16, sizeof(Response));
    if (!responses) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(HXC::NVS_Base::setup());
    RuntimeSettings::init();
    ESP_ERROR_CHECK(WiFiManager::instance().init());
    ESP_ERROR_CHECK(EspNowService::init());
    EspNowService::set_switch_response_handler(receive_switch);
    EspNowService::set_data_received_handler(receive_data);
    ESP_ERROR_CHECK(EspNowLink::register_handler(kDetailMessage, receive_detail));
    ESP_ERROR_CHECK(WiFiManager::instance().start_sta_radio(1));
    return xTaskCreate(worker, "emergency_remote", 5120, nullptr, 5, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
void IRAM_ATTR stop_from_isr() { contact_fell.store(true, std::memory_order_relaxed); }
void request_stop() { stop_requested.store(true); }
void request_on() { on_requested.store(true); }
void start_pairing(bool clear_first) { repair_requested.store(clear_first); pair_requested.store(true); }
bool prepare_sleep() {
    pause_requested.store(true);
    for (int i = 0; i < 10; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (snapshot().quiesced) return true;
    }
    pause_requested.store(false); return false;
}
void cancel_sleep() { pause_requested.store(false); }
void retry_connection() { retry_requested.store(true); }
void report_battery(uint8_t percent) {
    EspNowLink::SavedPeer peer{};
    const auto s = snapshot();
    if (!s.online || s.connection_failed || EspNowLink::get_saved_peer(0, &peer) != ESP_OK) return;
    ESP_LOGI(TAG, "BATTERY_REPORT percent=%u submit=%s", percent,
        esp_err_to_name(EspNowService::send_remote_battery(peer.address, percent)));
}
void test_wrong_channel() { wrong_channel_requested.store(true); }
Snapshot snapshot() {
    portENTER_CRITICAL(&lock); Snapshot copy = model; portEXIT_CRITICAL(&lock);
    copy.busy = copy.busy || contact_fell.load() || stop_requested.load() ||
        on_requested.load() || pair_requested.load() || retry_requested.load();
    copy.quiesced = copy.quiesced && !copy.busy &&
        copy.stop_closed == (gpio_get_level(GPIO_NUM_5) == 0);
    copy.online = copy.data_time_us > 0 && esp_timer_get_time() - copy.data_time_us < RuntimeSettings::get("fresh_ms") * 1000LL;
    return copy;
}
}
