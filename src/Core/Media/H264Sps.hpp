#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Acheron {
namespace Core {
namespace Media {
namespace H264 {

// the official client rewrites, before decrypting, the SPS of any H264 whose VUI bitstream restriction
// doesnt say there is no reordering and no more pictures held than referenced. the rewritten frame
// no longer matches what was encrypted and is never shown, so the same change is made here first

struct SpsReordering
{
    bool hasVui = false;
    bool hasBitstreamRestriction = false;
    uint32_t maxNumReorderFrames = 0;
    uint32_t maxDecFrameBuffering = 0;
    uint32_t maxNumRefFrames = 0;

    [[nodiscard]] bool isKeptByOfficialClient() const { return hasBitstreamRestriction && maxNumReorderFrames == 0 && maxDecFrameBuffering <= maxNumRefFrames; }
    bool operator==(const SpsReordering &) const = default;
};

[[nodiscard]] std::optional<SpsReordering> spsReordering(std::span<const uint8_t> sps);

[[nodiscard]] std::optional<std::vector<uint8_t>> rewrittenSps(std::span<const uint8_t> sps);

bool rewriteSpsIn(std::vector<uint8_t> &accessUnit);

[[nodiscard]] std::vector<uint8_t> withoutEmulationPrevention(std::span<const uint8_t> nal);
[[nodiscard]] std::vector<uint8_t> withEmulationPrevention(std::span<const uint8_t> payload);

} // namespace H264
} // namespace Media
} // namespace Core
} // namespace Acheron
