#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Acheron {
namespace Core {
namespace Media {
namespace H264 {

// profile_idc, which is also ffmpeg's profile value; ffmpeg renamed its own constants between versions
inline constexpr int PROFILE_BASELINE = 66;
inline constexpr int PROFILE_MAIN = 77;
inline constexpr int PROFILE_HIGH = 100;

// the official client rewrites, before decrypting, an H264 SPS whose VUI doesnt rule out reordering and holding
// more pictures than referenced; the frame then fails decryption and is never shown, so it is rewritten here first
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
