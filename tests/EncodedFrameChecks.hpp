#pragma once

#include "Discord/Voice/VideoFrame.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace EncodedFrameChecks {

using Acheron::Discord::Voice::EncodedVideoFrame;
using Acheron::Discord::Voice::VideoCodec;

inline bool startsWithStartCode(const std::vector<uint8_t> &data)
{
    if (data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return true;
    return data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

inline std::vector<int> nalTypes(VideoCodec codec, const std::vector<uint8_t> &data)
{
    std::vector<int> types;
    for (size_t i = 0; i + 3 < data.size(); i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            types.push_back(codec == VideoCodec::H265 ? (data[i + 3] >> 1) & 0x3F : data[i + 3] & 0x1F);
    }
    return types;
}

inline constexpr int H264_SPS = 7;
inline constexpr int H264_PPS = 8;
inline constexpr int H264_IDR = 5;
inline constexpr int H265_VPS = 32;
inline constexpr int H265_SPS = 33;
inline constexpr int H265_PPS = 34;
inline constexpr int H265_FIRST_IRAP = 16;
inline constexpr int H265_LAST_IRAP = 23;
inline constexpr int OBU_SEQUENCE_HEADER = 1;
inline constexpr int OBU_TEMPORAL_DELIMITER = 2;
inline constexpr int OBU_FRAME_HEADER = 3;
inline constexpr int OBU_FRAME = 6;

struct Obu
{
    int type = 0;
    bool saysItsSize = false;
    size_t payload = 0;
    size_t size = 0;
};

inline std::vector<Obu> obusOf(const std::vector<uint8_t> &data)
{
    std::vector<Obu> obus;
    for (size_t at = 0; at < data.size();) {
        Obu obu;
        obu.type = (data[at] >> 3) & 0x0F;
        obu.saysItsSize = data[at] & 0x02;
        obu.payload = at + ((data[at] & 0x04) ? 2 : 1);
        if ((data[at] & 0x80) || obu.payload > data.size())
            return {};
        obu.size = data.size() - obu.payload;
        if (obu.saysItsSize) {
            obu.size = 0;
            uint8_t byte = 0x80;
            for (int shift = 0; byte & 0x80; shift += 7) {
                if (obu.payload >= data.size() || shift > 28)
                    return {};
                byte = data[obu.payload++];
                obu.size |= size_t(byte & 0x7F) << shift;
            }
        }
        if (obu.size > data.size() - obu.payload)
            return {};
        obus.push_back(obu);
        at = obu.payload + obu.size;
    }
    return obus;
}

inline bool isWellFormed(VideoCodec codec, const EncodedVideoFrame &frame)
{
    if (frame.data.empty())
        return false;
    if (codec == VideoCodec::VP8)
        return !(frame.data[0] & 1) == frame.keyframe;

    if (codec == VideoCodec::AV1) {
        bool hasSequenceHeader = false;
        const Obu *picture = nullptr;
        const std::vector<Obu> obus = obusOf(frame.data);
        for (const Obu &obu : obus) {
            if (!obu.saysItsSize)
                return false;
            if (obu.type == OBU_SEQUENCE_HEADER && !picture)
                hasSequenceHeader = true;
            if ((obu.type == OBU_FRAME || obu.type == OBU_FRAME_HEADER) && !picture)
                picture = &obu;
        }
        if (!picture || picture->size == 0)
            return false;
        // show_existing_frame, frame_type (two bits, 0 is a key frame), show_frame
        const uint8_t opening = frame.data[picture->payload];
        const bool keyFrame = !(opening & 0x80) && ((opening >> 5) & 0x03) == 0 && (opening & 0x10);
        return keyFrame == frame.keyframe && (!keyFrame || hasSequenceHeader);
    }

    const bool h265 = codec == VideoCodec::H265;
    const std::vector<int> types = nalTypes(codec, frame.data);
    const bool hasKeyPicture = std::any_of(types.begin(), types.end(), [h265](int type) { return h265 ? type >= H265_FIRST_IRAP && type <= H265_LAST_IRAP : type == H264_IDR; });
    if (!startsWithStartCode(frame.data) || hasKeyPicture != frame.keyframe)
        return false;
    if (!frame.keyframe)
        return true;
    if (h265)
        return types.size() >= 4 && types[0] == H265_VPS && types[1] == H265_SPS && types[2] == H265_PPS;
    return types.size() >= 3 && types[0] == H264_SPS && types[1] == H264_PPS;
}

} // namespace EncodedFrameChecks
