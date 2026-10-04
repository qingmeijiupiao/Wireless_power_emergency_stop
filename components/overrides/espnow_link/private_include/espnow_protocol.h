#ifndef ESPNOW_PROTOCOL_H
#define ESPNOW_PROTOCOL_H

#include <cstddef>
#include <cstdint>

#include "espnow_link.h"

namespace EspNowLink::Internal {

constexpr uint16_t FRAME_MAGIC       = 0x5848;
constexpr uint8_t  FRAME_VERSION     = 1;
constexpr size_t   FRAME_HEADER_SIZE = 20;
constexpr uint16_t ACK_MESSAGE_ID    = 0;

enum FrameFlag : uint8_t {
    FRAME_FLAG_RELIABLE = 1U << 0,
    FRAME_FLAG_ACK      = 1U << 1,
};

struct ParsedFrame {
    uint8_t        flags        = 0;
    uint16_t       message_id   = 0;
    uint16_t       payload_size = 0;
    uint32_t       sequence     = 0;
    uint32_t       correlation  = 0;
    const uint8_t* payload      = nullptr;
};

/** @brief 快速检查帧长度、魔数、版本和固定头字段。 */
bool      quick_validate_frame(const uint8_t* data, size_t size);
/** @brief 校验并解析完整链路帧。 */
bool      decode_frame(const uint8_t* data, size_t size, ParsedFrame* frame);
/** @brief 按链路协议编码完整帧。 */
esp_err_t encode_frame(uint8_t flags, uint16_t message_id, uint32_t sequence, uint32_t correlation,
                       const uint8_t* payload, size_t payload_size, uint8_t* output, size_t output_capacity,
                       size_t* output_size);

} // namespace EspNowLink::Internal

#endif
