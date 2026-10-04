#include "espnow_link.h"
#include "esp_attr.h"

#include <algorithm>
#include <cstring>

#include "esp_now.h"
#include "espnow_link_internal.h"
#include "espnow_protocol.h"

namespace EspNowLink {

namespace {
portMUX_TYPE cancellation_lock = portMUX_INITIALIZER_UNLOCKED;
}
uint32_t IRAM_ATTR Internal::get_transmission_generation() {
    portENTER_CRITICAL(&cancellation_lock);
    const uint32_t value = transmission_generation;
    portEXIT_CRITICAL(&cancellation_lock);
    return value;
}
void cancel_transmissions() {
    portENTER_CRITICAL(&cancellation_lock);
    ++Internal::transmission_generation;
    portEXIT_CRITICAL(&cancellation_lock);
}
void IRAM_ATTR cancel_transmissions_from_isr() {
    portENTER_CRITICAL_ISR(&cancellation_lock);
    ++Internal::transmission_generation;
    portEXIT_CRITICAL_ISR(&cancellation_lock);
}

esp_err_t add_peer(const PeerConfig& config) {
    using namespace Internal;
    if (config.address.is_broadcast()) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&state_lock);
    PeerEntry* entry = find_peer(config.address);
    if (entry == nullptr) {
        for (auto& candidate : peers) {
            if (!candidate.used) {
                entry = &candidate;
                break;
            }
        }
    }
    if (entry == nullptr) {
        portEXIT_CRITICAL(&state_lock);
        return ESP_ERR_NO_MEM;
    }

    entry->used   = true;
    entry->config = config;
    if (entry->metrics.ack_timeout_ms == 0) {
        entry->metrics.ack_timeout_ms = DEFAULT_ACK_TIMEOUT_MS;
    }
    portEXIT_CRITICAL(&state_lock);

    if (!active) {
        return ESP_OK;
    }

    // peer.channel=0 表示始终跟随当前 STA/AP 工作信道。
    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, config.address.bytes, MAC_ADDRESS_SIZE);
    memcpy(info.lmk, config.lmk, KEY_SIZE);
    info.channel = 0;
    info.ifidx   = WIFI_IF_STA;
    info.encrypt = config.encrypted;
    return esp_now_is_peer_exist(config.address.bytes) ? esp_now_mod_peer(&info) : esp_now_add_peer(&info);
}

esp_err_t remove_peer(const MacAddress& address) {
    using namespace Internal;
    portENTER_CRITICAL(&state_lock);
    PeerEntry* peer = find_peer(address);
    if (peer == nullptr) {
        portEXIT_CRITICAL(&state_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *peer = {};
    portEXIT_CRITICAL(&state_lock);

    if (active && esp_now_is_peer_exist(address.bytes)) {
        esp_now_del_peer(address.bytes);
    }
    return ESP_OK;
}

bool has_peer(const MacAddress& address) {
    portENTER_CRITICAL(&Internal::state_lock);
    const bool found = Internal::find_peer(address) != nullptr;
    portEXIT_CRITICAL(&Internal::state_lock);
    return found;
}

esp_err_t send(const MacAddress& destination, uint16_t message_id, const void* payload, size_t payload_size,
               const SendOptions& options, SendCallback callback, void* context) {
    using namespace Internal;
    if (!initialized || !active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload_size > MAX_PAYLOAD_SIZE || (payload_size > 0 && payload == nullptr) || message_id == ACK_MESSAGE_ID) {
        return ESP_ERR_INVALID_ARG;
    }

    SendRequest request  = {};
    request.generation = get_transmission_generation();
    request.destination  = destination;
    request.message_id   = message_id;
    request.payload_size = static_cast<uint16_t>(payload_size);
    if (payload_size > 0) {
        memcpy(request.payload, payload, payload_size);
    }
    request.options  = options;
    request.callback = callback;
    request.context  = context;

    // 广播无法使用 ESP-NOW 加密，也不存在唯一 ACK 对端，强制使用 BEST_EFFORT。
    if (destination.is_broadcast()) {
        request.options.delivery        = Delivery::BEST_EFFORT;
        request.options.allow_plaintext = true;
    } else {
        portENTER_CRITICAL(&state_lock);
        PeerEntry* peer = find_peer(destination);
        if (peer == nullptr) {
            portEXIT_CRITICAL(&state_lock);
            return ESP_ERR_NOT_FOUND;
        }
        const bool encrypted = peer->config.encrypted;
        portEXIT_CRITICAL(&state_lock);
        if (!encrypted && !request.options.allow_plaintext) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (request.options.max_attempts == 0) {
        request.options.max_attempts = default_reliable_options.max_attempts;
    }

    return xQueueSend(tx_queue, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t register_handler(uint16_t message_id, MessageHandler handler, void* context) {
    if (message_id == Internal::ACK_MESSAGE_ID || handler == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&Internal::state_lock);
    for (auto& entry : Internal::handlers) {
        if (entry.used && entry.message_id == message_id) {
            entry.handler = handler;
            entry.context = context;
            portEXIT_CRITICAL(&Internal::state_lock);
            return ESP_OK;
        }
    }
    for (auto& entry : Internal::handlers) {
        if (!entry.used) {
            entry = {true, message_id, handler, context};
            portEXIT_CRITICAL(&Internal::state_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&Internal::state_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t unregister_handler(uint16_t message_id) {
    portENTER_CRITICAL(&Internal::state_lock);
    for (auto& entry : Internal::handlers) {
        if (entry.used && entry.message_id == message_id) {
            entry = {};
            portEXIT_CRITICAL(&Internal::state_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&Internal::state_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t set_default_reliable_options(const SendOptions& options) {
    if (options.max_attempts == 0 ||
        (options.ack_timeout_ms != 0 && (options.ack_timeout_ms < Internal::MIN_ACK_TIMEOUT_MS ||
                                         options.ack_timeout_ms > Internal::MAX_ACK_TIMEOUT_MS))) {
        return ESP_ERR_INVALID_ARG;
    }
    Internal::default_reliable_options          = options;
    Internal::default_reliable_options.delivery = Delivery::RELIABLE;
    return ESP_OK;
}

esp_err_t set_peer_ack_timeout(const MacAddress& address, uint16_t timeout_ms) {
    if (timeout_ms < Internal::MIN_ACK_TIMEOUT_MS || timeout_ms > Internal::MAX_ACK_TIMEOUT_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&Internal::state_lock);
    Internal::PeerEntry* peer = Internal::find_peer(address);
    if (peer == nullptr) {
        portEXIT_CRITICAL(&Internal::state_lock);
        return ESP_ERR_NOT_FOUND;
    }
    peer->metrics.ack_timeout_ms = timeout_ms;
    portEXIT_CRITICAL(&Internal::state_lock);
    return ESP_OK;
}

esp_err_t get_peer_metrics(const MacAddress& address, PeerMetrics* metrics) {
    if (metrics == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&Internal::state_lock);
    Internal::PeerEntry* peer = Internal::find_peer(address);
    if (peer == nullptr) {
        portEXIT_CRITICAL(&Internal::state_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *metrics = peer->metrics;
    portEXIT_CRITICAL(&Internal::state_lock);
    return ESP_OK;
}

uint32_t get_response_timeout_ms(const MacAddress& address, uint8_t request_attempts, uint8_t response_attempts,
                                 uint16_t processing_budget_ms) {
    PeerMetrics metrics = {};
    uint32_t    rto_ms  = Internal::DEFAULT_ACK_TIMEOUT_MS;
    if (get_peer_metrics(address, &metrics) == ESP_OK && metrics.ack_timeout_ms != 0) {
        rto_ms = metrics.ack_timeout_ms;
    }
    const uint32_t attempts = static_cast<uint32_t>(request_attempts) + response_attempts;
    return rto_ms * attempts + processing_budget_ms + 20U;
}

uint32_t get_delivery_timeout_ms(const MacAddress& address, uint8_t max_attempts, uint16_t mac_completion_budget_ms) {
    PeerMetrics metrics = {};
    uint32_t    rto_ms  = Internal::DEFAULT_ACK_TIMEOUT_MS;
    if (get_peer_metrics(address, &metrics) == ESP_OK && metrics.ack_timeout_ms != 0) {
        rto_ms = metrics.ack_timeout_ms;
    }
    return static_cast<uint32_t>(max_attempts) * (mac_completion_budget_ms + rto_ms) + 20U;
}

uint16_t get_channel_probe_timeout_ms(const MacAddress& address) {
    PeerMetrics metrics = {};
    if (get_peer_metrics(address, &metrics) != ESP_OK || metrics.rtt_sample_count <= 5 ||
        metrics.recent_rtt_count < 3) {
        return 30;
    }
    const uint32_t sum =
        static_cast<uint32_t>(metrics.recent_rtt_ms[0]) + metrics.recent_rtt_ms[1] + metrics.recent_rtt_ms[2];
    const uint32_t timeout_ms = (sum + 1U) / 2U;
    return static_cast<uint16_t>(std::clamp<uint32_t>(timeout_ms, 20U, 200U));
}

void get_statistics(LinkStatistics* output) {
    if (output == nullptr) {
        return;
    }
    portENTER_CRITICAL(&Internal::statistics_lock);
    *output = Internal::statistics;
    portEXIT_CRITICAL(&Internal::statistics_lock);
}

void reset_statistics() {
    portENTER_CRITICAL(&Internal::statistics_lock);
    Internal::statistics = {};
    portEXIT_CRITICAL(&Internal::statistics_lock);
}

} // namespace EspNowLink
