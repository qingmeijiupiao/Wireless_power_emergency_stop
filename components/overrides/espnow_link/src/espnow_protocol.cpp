#include "espnow_protocol.h"

#include <cstring>

#include "espnow_codec.h"

namespace EspNowLink::Internal {
namespace {

uint32_t update_crc32(uint32_t crc, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

uint32_t frame_crc32(const uint8_t* data, size_t payload_size) {
    uint32_t crc = update_crc32(0xffffffffU, data, 16);
    crc          = update_crc32(crc, data + FRAME_HEADER_SIZE, payload_size);
    return ~crc;
}

} // namespace

bool quick_validate_frame(const uint8_t* data, size_t size) {
    // 快速路径只读取固定头字段，不计算 CRC，不访问 peer 表。
    if (data == nullptr || size < FRAME_HEADER_SIZE || size > 250) {
        return false;
    }
    if (Codec::load_le<uint16_t>(data) != FRAME_MAGIC || data[2] != FRAME_VERSION) {
        return false;
    }
    if ((data[3] & ~(FRAME_FLAG_RELIABLE | FRAME_FLAG_ACK)) != 0) {
        return false;
    }
    const uint16_t payload_size = Codec::load_le<uint16_t>(data + 6);
    return payload_size <= MAX_PAYLOAD_SIZE && FRAME_HEADER_SIZE + payload_size == size;
}

bool decode_frame(const uint8_t* data, size_t size, ParsedFrame* frame) {
    if (frame == nullptr || !quick_validate_frame(data, size)) {
        return false;
    }

    const uint16_t payload_size = Codec::load_le<uint16_t>(data + 6);
    // 完整路径校验 CRC32，内容错误统一计入非法链路包。
    if (Codec::load_le<uint32_t>(data + 16) != frame_crc32(data, payload_size)) {
        return false;
    }

    frame->flags        = data[3];
    frame->message_id   = Codec::load_le<uint16_t>(data + 4);
    frame->payload_size = Codec::load_le<uint16_t>(data + 6);
    frame->sequence     = Codec::load_le<uint32_t>(data + 8);
    frame->correlation  = Codec::load_le<uint32_t>(data + 12);
    frame->payload      = data + FRAME_HEADER_SIZE;

    const bool ack = (frame->flags & FRAME_FLAG_ACK) != 0;
    if (ack && (frame->message_id != ACK_MESSAGE_ID || frame->payload_size != 0 ||
                (frame->flags & FRAME_FLAG_RELIABLE) != 0)) {
        return false;
    }
    return !ack || frame->correlation != 0;
}

esp_err_t encode_frame(uint8_t flags, uint16_t message_id, uint32_t sequence, uint32_t correlation,
                       const uint8_t* payload, size_t payload_size, uint8_t* output, size_t output_capacity,
                       size_t* output_size) {
    if (output == nullptr || output_size == nullptr || payload_size > MAX_PAYLOAD_SIZE ||
        (payload_size > 0 && payload == nullptr) || output_capacity < FRAME_HEADER_SIZE + payload_size) {
        return ESP_ERR_INVALID_ARG;
    }

    Codec::store_le<uint16_t>(output, FRAME_MAGIC);
    output[2] = FRAME_VERSION;
    output[3] = flags;
    Codec::store_le<uint16_t>(output + 4, message_id);
    Codec::store_le<uint16_t>(output + 6, static_cast<uint16_t>(payload_size));
    Codec::store_le<uint32_t>(output + 8, sequence);
    Codec::store_le<uint32_t>(output + 12, correlation);
    Codec::store_le<uint32_t>(output + 16, 0);
    if (payload_size > 0) {
        memcpy(output + FRAME_HEADER_SIZE, payload, payload_size);
    }
    Codec::store_le<uint32_t>(output + 16, frame_crc32(output, payload_size));
    *output_size = FRAME_HEADER_SIZE + payload_size;
    return ESP_OK;
}

} // namespace EspNowLink::Internal
