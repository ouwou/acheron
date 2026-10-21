#pragma once

#include <QString>

#include <array>
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
constexpr size_t VIDEO_CODEC_COUNT = static_cast<size_t>(VideoCodec::AV1) + 1;
inline constexpr std::array<VideoCodec, 4> VIDEO_CODECS_BEST_FIRST = { VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 };

// a VP8 stream cant say its colours, and decoders take it for BT.601
[[nodiscard]] constexpr bool carriesOnlyBt601(VideoCodec codec)
{
    return codec == VideoCodec::VP8;
}

inline constexpr int FULL_QUALITY = 100;
inline constexpr int SECOND_LAYER_QUALITY = 50;

// as the voice gateway spells them
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

struct EncodedVideoFrame
{
    std::vector<uint8_t> data;
    uint32_t rtpTimestamp = 0;
    bool keyframe = false;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
