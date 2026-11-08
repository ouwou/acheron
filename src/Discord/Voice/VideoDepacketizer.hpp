#pragma once

#include <QString>

#include <cstdint>
#include <span>
#include <vector>

#include "VideoFrame.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// not necessarily the frame's first packet: a sender may put an access unit delimiter ahead
[[nodiscard]] bool startsKeyframe(VideoCodec codec, std::span<const uint8_t> payload);

[[nodiscard]] bool startsWithParameterSets(VideoCodec codec, std::span<const uint8_t> payload);

[[nodiscard]] QString describeFrame(VideoCodec codec, std::span<const uint8_t> frame);

// byte for byte what the sender's frame encryptor produced, which is what the DAVE decryptor needs
[[nodiscard]] bool assembleFrame(VideoCodec codec, std::span<const std::span<const uint8_t>> payloads, std::vector<uint8_t> &frame);

void prepareForDecoder(VideoCodec codec, std::vector<uint8_t> &frame);

} // namespace Voice
} // namespace Discord
} // namespace Acheron
