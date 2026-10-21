#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Acheron {
namespace Discord {
namespace Voice {

inline void appendBytes(std::vector<uint8_t> &out, std::span<const uint8_t> bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// RFC 6184 and RFC 7798
inline constexpr uint8_t FU_START = 0x80;
inline constexpr uint8_t FU_END = 0x40;
inline constexpr size_t AGGREGATED_LENGTH_SIZE = 2;

// RFC 6184
inline constexpr uint8_t H264_STAP_A = 24;
inline constexpr uint8_t H264_FU_A = 28;

// RFC 7798
inline constexpr uint8_t H265_AP = 48;
inline constexpr uint8_t H265_FU = 49;
inline constexpr size_t H265_NAL_HEADER_SIZE = 2;

// RFC 7741, 4.2
inline constexpr uint8_t VP8_EXTENDED = 0x80;
inline constexpr uint8_t VP8_PARTITION_START = 0x10;
inline constexpr uint8_t VP8_PARTITION_ID_MASK = 0x07;
inline constexpr uint8_t VP8_HAS_PICTURE_ID = 0x80;
inline constexpr uint8_t VP8_HAS_TL0_PICTURE_INDEX = 0x40;
inline constexpr uint8_t VP8_HAS_TEMPORAL_ID_OR_KEY_INDEX = 0x30;
inline constexpr uint8_t VP8_LONG_PICTURE_ID = 0x80;

// https://aomediacodec.github.io/av1-rtp-spec/
inline constexpr uint8_t AV1_CONTINUES_PREVIOUS = 0x80;
inline constexpr uint8_t AV1_CONTINUES_IN_NEXT = 0x40;
inline constexpr uint8_t AV1_NEW_SEQUENCE = 0x08;
inline constexpr int AV1_ELEMENT_COUNT_SHIFT = 4;
inline constexpr uint8_t AV1_ELEMENT_COUNT_MASK = 0x03;

} // namespace Voice
} // namespace Discord
} // namespace Acheron
