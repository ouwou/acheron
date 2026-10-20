#pragma once

#include <QString>

#include <cstdint>
#include <span>
#include <vector>

#include "VideoFrame.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// Whether an RTP payload carries the beginning of a key frame: its parameter sets, or the start
// of its picture. It need not be the frame's first packet, since a sender may put other units,
// such as an access unit delimiter, ahead of it.
[[nodiscard]] bool startsKeyframe(VideoCodec codec, std::span<const uint8_t> payload);

// Whether such a payload begins with the key frame's parameter sets, as opposed to its picture.
// A packet that only starts the picture may have had parameter sets in packets before it.
[[nodiscard]] bool startsWithParameterSets(VideoCodec codec, std::span<const uint8_t> payload);

// A one-line account of an assembled frame for the log: its units, and the end-to-end
// encryption trailer if it has one.
[[nodiscard]] QString describeFrame(VideoCodec codec, std::span<const uint8_t> frame);

// Rebuilds one encoded frame from its RTP payloads, given in sequence order. The result is byte
// for byte what the sender's frame encryptor produced, because that is what the end-to-end
// decryptor has to be handed: Annex B with a 4-byte start code on every NAL for H264 and H265,
// the bare frame for VP8, and for AV1 the OBUs with a size field on all but the last.
[[nodiscard]] bool assembleFrame(VideoCodec codec, std::span<const std::span<const uint8_t>> payloads, std::vector<uint8_t> &frame);

// Turns an assembled, decrypted frame into what a decoder accepts. Only AV1 needs anything: its
// last OBU is given the size field the sender left off.
void prepareForDecoder(VideoCodec codec, std::vector<uint8_t> &frame);

} // namespace Voice
} // namespace Discord
} // namespace Acheron
