#pragma once

#include <QString>

#include <cstdint>
#include <vector>

namespace Acheron {
namespace Discord {
namespace Voice {

enum class VideoCodec {
    Unknown,
    H264,
    H265,
    VP8,
    AV1,
};

// Codec names as the voice gateway spells them.
[[nodiscard]] inline VideoCodec videoCodecFromName(const QString &name)
{
    if (name == QLatin1String("H264"))
        return VideoCodec::H264;
    if (name == QLatin1String("H265"))
        return VideoCodec::H265;
    if (name == QLatin1String("VP8"))
        return VideoCodec::VP8;
    if (name == QLatin1String("AV1"))
        return VideoCodec::AV1;
    return VideoCodec::Unknown;
}

[[nodiscard]] inline QString videoCodecName(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264:
        return QStringLiteral("H264");
    case VideoCodec::H265:
        return QStringLiteral("H265");
    case VideoCodec::VP8:
        return QStringLiteral("VP8");
    case VideoCodec::AV1:
        return QStringLiteral("AV1");
    case VideoCodec::Unknown:
        break;
    }
    return {};
}

// One whole encoded frame: what an encoder hands the RTP sender, and what the RTP receiver
// hands a decoder. The timestamp is at 90 kHz.
struct EncodedVideoFrame
{
    std::vector<uint8_t> data;
    uint32_t rtpTimestamp = 0;
    bool keyframe = false;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
