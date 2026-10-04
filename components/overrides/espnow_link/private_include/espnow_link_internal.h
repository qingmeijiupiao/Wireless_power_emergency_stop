#ifndef ESPNOW_LINK_INTERNAL_H
#define ESPNOW_LINK_INTERNAL_H
#include "sdkconfig.h"

#include "esp_now.h"
#include "espnow_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace EspNowLink::Internal {

constexpr size_t      MAX_HANDLERS           = 16;
constexpr size_t      MAX_PEERS              = 7;
constexpr size_t      RX_QUEUE_LENGTH        = 16;
constexpr size_t      TX_QUEUE_LENGTH        = 16;
constexpr size_t      MAC_QUEUE_LENGTH       = 8;
constexpr size_t      ACK_QUEUE_LENGTH       = 8;
// 链路任务实测峰值约 1.4KB，保留超过 2KB 余量覆盖业务回调和重传路径。
constexpr uint32_t    TASK_STACK_SIZE        = CONFIG_ESPNOW_LINK_TASK_STACK_SIZE;
constexpr UBaseType_t TASK_PRIORITY          = 4;
constexpr uint16_t    DEFAULT_ACK_TIMEOUT_MS = 20;
constexpr uint16_t    MIN_ACK_TIMEOUT_MS     = 8;
constexpr uint16_t    MAX_ACK_TIMEOUT_MS     = 100;

struct HandlerEntry {
    bool           used       = false;
    uint16_t       message_id = 0;
    MessageHandler handler    = nullptr;
    void*          context    = nullptr;
};

struct PeerEntry {
    bool        used             = false;
    PeerConfig  config           = {};
    PeerMetrics metrics          = {};
    uint32_t    last_rx_session  = 0;
    uint32_t    last_rx_sequence = 0;
    bool        has_rx_sequence  = false;
};

struct RxEvent {
    MacAddress source      = {};
    MacAddress destination = {};
    int8_t     rssi        = 0;
    uint8_t    channel     = 0;
    uint16_t   size        = 0;
    uint8_t    data[250]   = {};
};

struct SendRequest {
    uint32_t     generation; /**< 发送所属取消代际，出队/重传前检查。 */
    MacAddress   destination;
    uint16_t     message_id;
    uint16_t     payload_size;
    uint8_t      payload[MAX_PAYLOAD_SIZE];
    SendOptions  options;
    SendCallback callback;
    void*        context;
};

struct MacResultEvent {
    MacAddress destination;
    bool       success;
};

struct AckRequest {
    MacAddress destination;
    uint32_t   session;
    uint32_t   sequence;
};

struct PendingTransmission {
    bool        active;
    bool        waiting_ack;
    bool        retransmitted;
    SendRequest request;
    uint32_t    sequence;
    uint8_t     attempts;
    TickType_t  first_send_tick;
    TickType_t  deadline_tick;
    size_t      frame_size;
    uint8_t     frame[250];
};

extern bool                initialized;
extern bool                active;
extern QueueHandle_t       rx_queue;
extern QueueHandle_t       tx_queue;
extern QueueHandle_t       mac_queue;
extern QueueHandle_t       ack_queue;
extern TaskHandle_t        task_handle;
extern HandlerEntry        handlers[MAX_HANDLERS];
extern PeerEntry           peers[MAX_PEERS];
extern PendingTransmission pending;
extern SendOptions         default_reliable_options;
extern LinkStatistics      statistics;
extern uint32_t            transmission_generation;
/** @brief 读取取消代际；临界区与 ISR 取消共用同一把短锁。 */
uint32_t get_transmission_generation();
extern uint32_t            next_sequence;
extern uint32_t            local_session_id;
extern portMUX_TYPE        statistics_lock;
extern portMUX_TYPE        state_lock;

/** @brief 查找指定 MAC 对应的运行期 peer。 */
PeerEntry* find_peer(const MacAddress& address);
/** @brief 在线程安全的临界区内递增统计计数器。 */
void       increment_counter(uint32_t* counter);
/** @brief 处理接收队列中的一个 ESP-NOW 帧。 */
void       process_received_event(const RxEvent& event);
/** @brief 处理驱动返回的 MAC 层发送结果。 */
void       process_mac_result(const MacResultEvent& event);
/** @brief 检查当前可靠发送是否超时并按策略重传。 */
void       process_timeout();
/** @brief ESP-NOW 链路任务入口。 */
void       link_task(void* context);
/** @brief 射频关闭时清除驱动所有权及旧队列；调用者必须独占/暂停链路任务。 */
void       reset_transmit_state();
/** @brief 在 WiFi 射频启动后激活 ESP-NOW 链路。 */
esp_err_t  activate();
/** @brief 在 WiFi 射频停止前停用 ESP-NOW 链路。 */
void       deactivate();

} // namespace EspNowLink::Internal

#endif
