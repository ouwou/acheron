#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Acheron {
namespace Discord {
namespace Voice {

inline constexpr std::array<uint8_t, 4> START_CODE = { 0, 0, 0, 1 };

// the NAL units of an Annex B stream, without their start codes. a zero byte before a three-byte start
// code belongs to the start code
[[nodiscard]] inline std::vector<std::span<const uint8_t>> splitAnnexB(std::span<const uint8_t> stream)
{
    constexpr size_t SHORT_START_CODE_SIZE = 3;

    std::vector<std::span<const uint8_t>> nals;
    size_t nalStart = 0;
    bool inNal = false;
    const auto endNal = [&](size_t end) {
        if (inNal && end > nalStart)
            nals.push_back(stream.subspan(nalStart, end - nalStart));
    };

    size_t i = 0;
    while (i + SHORT_START_CODE_SIZE <= stream.size()) {
        if (stream[i] != 0 || stream[i + 1] != 0 || stream[i + 2] != 1) {
            i++;
            continue;
        }
        endNal(i > nalStart && stream[i - 1] == 0 ? i - 1 : i);
        i += SHORT_START_CODE_SIZE;
        nalStart = i;
        inNal = true;
    }
    endNal(stream.size());
    return nals;
}

inline constexpr uint8_t H264_TYPE_MASK = 0x1F;
inline constexpr uint8_t H264_IDR = 5;
inline constexpr uint8_t H264_SPS = 7;
inline constexpr uint8_t H264_PPS = 8;
inline constexpr uint8_t H264_ACCESS_UNIT_DELIMITER = 9;

[[nodiscard]] constexpr uint8_t h264Type(uint8_t firstHeaderByte)
{
    return firstHeaderByte & H264_TYPE_MASK;
}

inline constexpr uint8_t H265_TYPE_BITS = 0x7E;
inline constexpr uint8_t H265_IRAP_FIRST = 16;
inline constexpr uint8_t H265_IRAP_LAST = 23;
inline constexpr uint8_t H265_VPS = 32;
inline constexpr uint8_t H265_SPS = 33;
inline constexpr uint8_t H265_PPS = 34;
inline constexpr uint8_t H265_ACCESS_UNIT_DELIMITER = 35;

[[nodiscard]] constexpr uint8_t h265Type(uint8_t firstHeaderByte)
{
    return (firstHeaderByte & H265_TYPE_BITS) >> 1;
}

[[nodiscard]] constexpr bool h265IsIrap(uint8_t type)
{
    return type >= H265_IRAP_FIRST && type <= H265_IRAP_LAST;
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
