#include "espnow_link.h"

#include <algorithm>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "diagnostic_log.h"
#include "esp_now.h"
#include "esp_random.h"
#include "espnow_link_internal.h"
#include "espnow_pairing_internal.h"
#include "espnow_protocol.h"
#include "wifi_manager.h"

namespace EspNowLink {

const MacAddress BROADCAST_ADDRESS = {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};

bool MacAddress::operator==(const MacAddress& other) const {
    return memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
}

bool MacAddress::operator!=(const MacAddress& other) const {
    return !(*this == other);
}

bool MacAddress::is_broadcast() const {
    return *this == BROADCAST_ADDRESS;
}

namespace Internal {

static constexpr char TAG[] = "EspNowLink";

bool                initialized              = false;
bool                active                   = false;
QueueHandle_t       rx_queue                 = nullptr;
QueueHandle_t       tx_queue                 = nullptr;
QueueHandle_t       mac_queue                = nullptr;
QueueHandle_t       ack_queue                = nullptr;
TaskHandle_t        task_handle              = nullptr;
HandlerEntry        handlers[MAX_HANDLERS]   = {};
PeerEntry           peers[MAX_PEERS]         = {};
PendingTransmission pending                  = {};
SendOptions         default_reliable_options = {};
LinkStatistics      statistics               = {};
uint32_t            transmission_generation = 1;
uint32_t            next_sequence            = 1;
uint32_t            local_session_id         = 1;
portMUX_TYPE        statistics_lock          = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE        state_lock               = portMUX_INITIALIZER_UNLOCKED;

void increment_counter(uint32_t* counter) {
    portENTER_CRITICAL(&statistics_lock);
    (*counter)++;
    portEXIT_CRITICAL(&statistics_lock);
}

PeerEntry* find_peer(const MacAddress& address) {
    for (auto& peer : peers) {
        if (peer.used && peer.config.address == address) {
            return &peer;
        }
    }
    return nullptr;
}

static void receive_callback(const esp_now_recv_info_t* info, const uint8_t* data, int data_len) {
    // WiFi 高优先级任务中只做固定头快速检查，非法帧不进入应用队列。
    if (info == nullptr || info->src_addr == nullptr || info->des_addr == nullptr ||
        !quick_validate_frame(data, data_len)) {
        increment_counter(&statistics.rx_invalid_packets);
        return;
    }

    RxEvent event = {};
    memcpy(event.source.bytes, info->src_addr, MAC_ADDRESS_SIZE);
    memcpy(event.destination.bytes, info->des_addr, MAC_ADDRESS_SIZE);
    event.size = static_cast<uint16_t>(data_len);
    memcpy(event.data, data, event.size);
    if (info->rx_ctrl != nullptr) {
        event.rssi    = info->rx_ctrl->rssi;
        event.channel = info->rx_ctrl->channel;
    }
    // 队列保存完整帧副本，避免 IDF 回调返回后继续引用临时接收缓冲。
    if (rx_queue == nullptr || xQueueSend(rx_queue, &event, 0) != pdTRUE) {
        increment_counter(&statistics.rx_queue_overflows);
    }
}

static void send_callback(const esp_now_send_info_t* info, esp_now_send_status_t status) {
    if (mac_queue == nullptr || info == nullptr || info->des_addr == nullptr) {
        return;
    }
    MacResultEvent event = {};
    memcpy(event.destination.bytes, info->des_addr, MAC_ADDRESS_SIZE);
    event.success = status == ESP_NOW_SEND_SUCCESS;
    // 发送完成结果交给链路任务串行处理，不在 WiFi 任务中推进重传状态机。
    xQueueSend(mac_queue, &event, 0);
}

esp_err_t activate() {
    if (!initialized || active) {
        return initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    // WiFi 射频启动后初始化 ESP-NOW，并重新安装广播 peer 与已保存单播 peer。
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp_now_init failed");
    esp_err_t ret = esp_now_register_recv_cb(receive_callback);
    if (ret == ESP_OK) {
        ret = esp_now_register_send_cb(send_callback);
    }
    if (ret != ESP_OK) {
        esp_now_deinit();
        return ret;
    }

    esp_now_peer_info_t broadcast = {};
    memcpy(broadcast.peer_addr, BROADCAST_ADDRESS.bytes, MAC_ADDRESS_SIZE);
    broadcast.channel = 0;
    broadcast.ifidx   = WIFI_IF_STA;
    ret               = esp_now_add_peer(&broadcast);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();
        return ret;
    }

    for (const auto& peer : peers) {
        if (!peer.used) {
            continue;
        }
        esp_now_peer_info_t info = {};
        memcpy(info.peer_addr, peer.config.address.bytes, MAC_ADDRESS_SIZE);
        memcpy(info.lmk, peer.config.lmk, KEY_SIZE);
        info.channel = 0;
        info.ifidx   = WIFI_IF_STA;
        info.encrypt = peer.config.encrypted;
        ret          = esp_now_add_peer(&info);
        if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGW(TAG, "restore peer failed: %s", esp_err_to_name(ret));
        }
    }

    active = true;
    DEVICE_STATE_I(TAG, "espnow: link old=inactive new=active result=ok");
    return ESP_OK;
}

void deactivate() {
    if (!active) {
        return;
    }
    // 射频生命周期操作独占链路状态。自身任务调用时不暂停自己。
    active = false;
    const bool suspend_worker = task_handle && task_handle != xTaskGetCurrentTaskHandle();
    if (suspend_worker) vTaskSuspend(task_handle);
    // 必须在 esp_wifi_stop() 前解除回调并反初始化，防止驱动重启后状态残留。
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    reset_transmit_state();
    if (suspend_worker) vTaskResume(task_handle);
    DEVICE_STATE_I(TAG, "espnow: link old=active new=inactive result=ok");
}

static void radio_event_handler(WiFiManager::RadioEvent event, void*) {
    if (event == WiFiManager::RadioEvent::AFTER_START) {
        const esp_err_t ret = activate();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "activation after WiFi start failed: %s", esp_err_to_name(ret));
        }
    } else {
        deactivate();
    }
}

} // namespace Internal

esp_err_t init() {
    using namespace Internal;
    if (initialized) {
        return ESP_OK;
    }

    // 所有队列使用固定元素大小，运行期不为单个收发包动态分配内存。
    rx_queue  = xQueueCreate(RX_QUEUE_LENGTH, sizeof(RxEvent));
    tx_queue  = xQueueCreate(TX_QUEUE_LENGTH, sizeof(SendRequest));
    mac_queue = xQueueCreate(MAC_QUEUE_LENGTH, sizeof(MacResultEvent));
    ack_queue = xQueueCreate(ACK_QUEUE_LENGTH, sizeof(AckRequest));
    if (rx_queue == nullptr || tx_queue == nullptr || mac_queue == nullptr || ack_queue == nullptr) {
        deinit();
        return ESP_ERR_NO_MEM;
    }

    default_reliable_options.delivery       = Delivery::RELIABLE;
    default_reliable_options.timeout_mode   = TimeoutMode::ADAPTIVE;
    default_reliable_options.ack_timeout_ms = DEFAULT_ACK_TIMEOUT_MS;
    default_reliable_options.max_attempts   = 5;
    // 每次启动生成新的 session，避免对端把本机重启后的低序号误判为旧包。
    local_session_id                        = esp_random();
    if (local_session_id == 0) {
        local_session_id = 1;
    }

    if (xTaskCreate(link_task, "espnow_link", TASK_STACK_SIZE, nullptr, TASK_PRIORITY, &task_handle) != pdPASS) {
        deinit();
        return ESP_ERR_NO_MEM;
    }

    initialized   = true;
    esp_err_t ret = WiFiManager::instance().register_radio_listener(radio_event_handler, nullptr);
    if (ret != ESP_OK) {
        deinit();
        return ret;
    }
    if (WiFiManager::instance().is_started()) {
        ret = activate();
    }
    if (ret == ESP_OK) {
        ret = init_pairing();
    }
    return ret;
}

esp_err_t deinit() {
    using namespace Internal;
    if (initialized) {
        WiFiManager::instance().unregister_radio_listener(radio_event_handler, nullptr);
    }
    deactivate();
    if (task_handle != nullptr) {
        vTaskDelete(task_handle);
        task_handle = nullptr;
    }
    if (rx_queue != nullptr) {
        vQueueDelete(rx_queue);
        rx_queue = nullptr;
    }
    if (tx_queue != nullptr) {
        vQueueDelete(tx_queue);
        tx_queue = nullptr;
    }
    if (mac_queue != nullptr) {
        vQueueDelete(mac_queue);
        mac_queue = nullptr;
    }
    if (ack_queue != nullptr) {
        vQueueDelete(ack_queue);
        ack_queue = nullptr;
    }
    initialized = false;
    return ESP_OK;
}

bool is_initialized() {
    return Internal::initialized;
}

bool is_active() {
    return Internal::active;
}

} // namespace EspNowLink
