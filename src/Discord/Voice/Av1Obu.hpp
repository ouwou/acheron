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

// the parts of an AV1 open bitstream unit's header that the packetizers and the encoder look at
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

// reads a size off the front of `bytes`, which is left after it. false when it doesnt end
// within the bytes, or within the most AV1 allows
inline bool readLeb128(std::span<const uint8_t> &bytes, size_t &value)
{
    value = 0;
    for (int i = 0; i < LEB128_MAX_BYTES; i++) {
        if (bytes.empty())
            return false;
        const uint8_t byte = bytes[0];
        bytes = bytes.subspan(1);
        value |= size_t(byte & 0x7F) << (7 * i);
        if (!(byte & 0x80))
            return true;
    }
    return false;
}

struct ParsedObu
{
    std::span<const uint8_t> header;
    std::span<const uint8_t> body;
    bool hasSize = false;

    [[nodiscard]] int type() const { return (header[0] >> 3) & 0x0F; }
};

// reads one OBU off the front of `bytes`. one that doesnt say its size runs to their end
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
    if (obu.hasSize && (!readLeb128(rest, size) || size > rest.size()))
        return std::nullopt;
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
