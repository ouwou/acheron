#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "VideoDepacketizer.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// as WebRTC writes them
class VideoPacketizer
{
public:
    [[nodiscard]] static bool supports(VideoCodec codec);

    explicit VideoPacketizer(VideoCodec codec, uint16_t firstPictureId = 0);

    [[nodiscard]] VideoCodec codec() const { return videoCodec; }

    // an AV1 frame's last OBU may lack its size, as the DAVE encryptor leaves it. `keyframe` only matters to
    // AV1, whose first packet says whether a new coded video sequence begins
    [[nodiscard]] std::vector<std::vector<uint8_t>> packetize(std::span<const uint8_t> frame, bool keyframe, int maxPayloadSize);

    static constexpr int MIN_PAYLOAD_SIZE = 16;

private:
    VideoCodec videoCodec;
    uint16_t pictureId;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
