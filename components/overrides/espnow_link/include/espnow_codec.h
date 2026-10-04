#ifndef ESPNOW_CODEC_H
#define ESPNOW_CODEC_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace EspNowLink::Codec {

/**
 * @brief 从协议字节流读取小端整数
 *
 * 逐字节读取不依赖 CPU 对齐、严格别名规则或本机端序，适用于任意 payload 偏移。
 */
template <typename T> T load_le(const uint8_t* data) {
    static_assert(std::is_integral_v<T>, "load_le only supports integral types");
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<Unsigned>(data[i]) << (i * 8U);
    }
    return static_cast<T>(value);
}

/**
 * @brief 将整数按小端格式写入协议字节流
 */
template <typename T> void store_le(uint8_t* data, T value) {
    static_assert(std::is_integral_v<T>, "store_le only supports integral types");
    using Unsigned         = std::make_unsigned_t<T>;
    const Unsigned encoded = static_cast<Unsigned>(value);
    for (size_t i = 0; i < sizeof(T); ++i) {
        data[i] = static_cast<uint8_t>(encoded >> (i * 8U));
    }
}

} // namespace EspNowLink::Codec

#endif
