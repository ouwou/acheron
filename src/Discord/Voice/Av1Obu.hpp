#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Acheron {
namespace Discord {
namespace Voice {
namespace Av1 {

constexpr uint8_t OBU_FORBIDDEN_BIT = 0x80;
constexpr uint8_t OBU_HAS_EXTENSION = 0x04;
constexpr uint8_t OBU_HAS_SIZE = 0x02;
constexpr int OBU_SEQUENCE_HEADER = 1;
constexpr int OBU_TEMPORAL_DELIMITER = 2;
constexpr int OBU_FRAME_HEADER = 3;
constexpr int OBU_FRAME = 6;
constexpr int OBU_TILE_LIST = 8;
constexpr int OBU_PADDING = 15;
constexpr int LEB128_MAX_BYTES = 8;

[[nodiscard]] inline std::optional<size_t> readLeb128(std::span<const uint8_t> &bytes)
{
    size_t value = 0;
    for (int i = 0; i < LEB128_MAX_BYTES; i++) {
        if (bytes.empty())
            return std::nullopt;
        const uint8_t byte = bytes[0];
        bytes = bytes.subspan(1);
        value |= size_t(byte & 0x7F) << (7 * i);
        if (!(byte & 0x80))
            return value;
    }
    return std::nullopt;
}

struct ParsedObu
{
    std::span<const uint8_t> header;
    std::span<const uint8_t> body;
    bool hasSize = false;

    [[nodiscard]] int type() const { return (header[0] >> 3) & 0x0F; }
};

// an OBU that doesnt state its size runs to the end of `bytes`
[[nodiscard]] inline std::optional<ParsedObu> readObu(std::span<const uint8_t> &bytes)
{
    if (bytes.empty())
        return std::nullopt;
    const size_t headerSize = (bytes[0] & OBU_HAS_EXTENSION) ? 2 : 1;
    if (bytes.size() < headerSize)
        return std::nullopt;

    ParsedObu obu;
    obu.header = bytes.first(headerSize);
    obu.hasSize = bytes[0] & OBU_HAS_SIZE;
    std::span<const uint8_t> rest = bytes.subspan(headerSize);
    size_t size = rest.size();
    if (obu.hasSize) {
        const std::optional<size_t> stated = readLeb128(rest);
        if (!stated || *stated > rest.size())
            return std::nullopt;
        size = *stated;
    }
    obu.body = rest.first(size);
    bytes = rest.subspan(size);
    return obu;
}

inline void appendLeb128(std::vector<uint8_t> &out, size_t value)
{
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value != 0)
            byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
}

inline size_t leb128Size(size_t value)
{
    size_t bytes = 1;
    while (value >>= 7)
        bytes++;
    return bytes;
}

} // namespace Av1
} // namespace Voice
} // namespace Discord
} // namespace Acheron
