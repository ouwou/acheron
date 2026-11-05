#include "Core/Media/H264Sps.hpp"

#include "Core/Media/BitReader.hpp"
#include "Discord/Voice/NalUnits.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace Acheron {
namespace Core {
namespace Media {
namespace H264 {

namespace {

constexpr uint8_t EMULATION_PREVENTION = 3;

constexpr std::array<uint32_t, 13> PROFILES_WITH_CHROMA_FORMAT = { 100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135 };
constexpr uint32_t CHROMA_444 = 3;

constexpr int NAL_HEADER_BITS = 8;
constexpr int PROFILE_BITS = 8;
constexpr int CONSTRAINT_FLAGS_AND_LEVEL_BITS = 16;
constexpr int ASPECT_RATIO_BITS = 8;
constexpr uint32_t EXTENDED_SAR = 255;
constexpr int SAR_BITS = 32;
constexpr int VIDEO_FORMAT_AND_RANGE_BITS = 4;
constexpr int COLOUR_DESCRIPTION_BITS = 24;
constexpr int TIMING_INFO_BITS = 65;
constexpr int HRD_SCALE_BITS = 8;
constexpr int HRD_LENGTH_BITS = 20;
constexpr int VUI_FLAGS_BEFORE_RESTRICTION = 8;

constexpr uint32_t MOST_PICTURES_IN_ORDER_CYCLE = 255;
constexpr uint32_t MOST_CODED_PICTURE_BUFFERS = 32;

// what the official client writes into a restriction it adds
constexpr bool ADDED_MOTION_VECTORS_OVER_PIC_BOUNDARIES = true;
constexpr uint32_t ADDED_MAX_BYTES_PER_PIC_DENOM = 2;
constexpr uint32_t ADDED_MAX_BITS_PER_MB_DENOM = 1;
constexpr uint32_t ADDED_LOG2_MAX_MV_LENGTH = 16;

constexpr uint32_t NO_REORDERING = 0;

class BitWriter
{
public:
    void put(uint64_t value, int count)
    {
        for (int i = count - 1; i >= 0; i--, used++) {
            if (used % 8 == 0)
                bytes.push_back(0);
            bytes.back() |= static_cast<uint8_t>(((value >> i) & 1) << (7 - used % 8));
        }
    }

    void flag(bool set) { put(set ? 1 : 0, 1); }

    void ue(uint32_t value)
    {
        const uint64_t code = uint64_t(value) + 1;
        const int length = std::bit_width(code);
        put(0, length - 1);
        put(code, length);
    }

    [[nodiscard]] std::vector<uint8_t> withTrailingBits()
    {
        flag(true);
        return std::move(bytes);
    }

private:
    std::vector<uint8_t> bytes;
    size_t used = 0;
};

void skipScalingList(BitReader &reader, int size)
{
    int lastScale = 8;
    int nextScale = 8;
    for (int i = 0; i < size; i++) {
        if (nextScale != 0) {
            const int32_t deltaScale = reader.se();
            if (deltaScale < -128 || deltaScale > 127)
                return reader.fail();
            nextScale = (lastScale + deltaScale) & 0xFF;
        }
        if (nextScale != 0)
            lastScale = nextScale;
    }
}

void skipHrdParameters(BitReader &reader)
{
    const uint32_t codedPictureBuffers = reader.ue() + 1;
    if (codedPictureBuffers > MOST_CODED_PICTURE_BUFFERS)
        return reader.fail();
    reader.skip(HRD_SCALE_BITS);
    for (uint32_t i = 0; i < codedPictureBuffers; i++) {
        reader.ue();
        reader.ue();
        reader.skip(1);
    }
    reader.skip(HRD_LENGTH_BITS);
}

struct ParsedSps
{
    std::vector<uint8_t> unescaped;
    SpsReordering reordering;
    size_t keptBits = 0;
};

// field by field as in H264 7.3.2.1.1, E.1.1 and E.1.2
std::optional<ParsedSps> parse(std::span<const uint8_t> sps)
{
    if (sps.empty() || Discord::Voice::h264Type(sps[0]) != Discord::Voice::H264_SPS)
        return std::nullopt;

    ParsedSps parsed;
    parsed.unescaped = withoutEmulationPrevention(sps);
    BitReader reader(parsed.unescaped);
    SpsReordering &reordering = parsed.reordering;

    reader.skip(NAL_HEADER_BITS);
    const uint32_t profile = reader.bits(PROFILE_BITS);
    reader.skip(CONSTRAINT_FLAGS_AND_LEVEL_BITS);
    reader.ue();
    if (std::ranges::find(PROFILES_WITH_CHROMA_FORMAT, profile) != PROFILES_WITH_CHROMA_FORMAT.end()) {
        const uint32_t chromaFormat = reader.ue();
        if (chromaFormat == CHROMA_444)
            reader.skip(1);
        reader.ue();
        reader.ue();
        reader.skip(1);
        const bool hasScalingMatrix = reader.flag();
        const int scalingLists = chromaFormat == CHROMA_444 ? 12 : 8;
        for (int i = 0; hasScalingMatrix && i < scalingLists; i++) {
            if (reader.flag())
                skipScalingList(reader, i < 6 ? 16 : 64);
        }
    }
    reader.ue();
    const uint32_t pictureOrderCountType = reader.ue();
    if (pictureOrderCountType == 0) {
        reader.ue();
    } else if (pictureOrderCountType == 1) {
        reader.skip(1);
        reader.se();
        reader.se();
        const uint32_t picturesInCycle = reader.ue();
        if (picturesInCycle > MOST_PICTURES_IN_ORDER_CYCLE)
            return std::nullopt;
        for (uint32_t i = 0; i < picturesInCycle; i++)
            reader.se();
    }
    reordering.maxNumRefFrames = reader.ue();
    reader.skip(1);
    reader.ue();
    reader.ue();
    const bool frameMbsOnly = reader.flag();
    if (!frameMbsOnly)
        reader.skip(1);
    reader.skip(1);
    const bool hasFrameCropping = reader.flag();
    for (int i = 0; hasFrameCropping && i < 4; i++)
        reader.ue();

    parsed.keptBits = reader.position();
    reordering.hasVui = reader.flag();
    if (reordering.hasVui) {
        const bool hasAspectRatio = reader.flag();
        if (hasAspectRatio && reader.bits(ASPECT_RATIO_BITS) == EXTENDED_SAR)
            reader.skip(SAR_BITS);
        const bool hasOverscanInfo = reader.flag();
        if (hasOverscanInfo)
            reader.skip(1);
        const bool hasVideoSignalType = reader.flag();
        if (hasVideoSignalType) {
            reader.skip(VIDEO_FORMAT_AND_RANGE_BITS);
            const bool hasColourDescription = reader.flag();
            if (hasColourDescription)
                reader.skip(COLOUR_DESCRIPTION_BITS);
        }
        const bool hasChromaLocation = reader.flag();
        if (hasChromaLocation) {
            reader.ue();
            reader.ue();
        }
        const bool hasTimingInfo = reader.flag();
        if (hasTimingInfo)
            reader.skip(TIMING_INFO_BITS);
        const bool hasNalHrd = reader.flag();
        if (hasNalHrd)
            skipHrdParameters(reader);
        const bool hasVclHrd = reader.flag();
        if (hasVclHrd)
            skipHrdParameters(reader);
        if (hasNalHrd || hasVclHrd)
            reader.skip(1);
        reader.skip(1);

        parsed.keptBits = reader.position();
        reordering.hasBitstreamRestriction = reader.flag();
        if (reordering.hasBitstreamRestriction) {
            reader.skip(1);
            reader.ue();
            reader.ue();
            reader.ue();
            reader.ue();
            parsed.keptBits = reader.position();
            reordering.maxNumReorderFrames = reader.ue();
            reordering.maxDecFrameBuffering = reader.ue();
        }
    }

    const bool hasStopBit = reader.flag();
    if (!hasStopBit || reader.failed())
        return std::nullopt;
    return parsed;
}

} // namespace

std::vector<uint8_t> withoutEmulationPrevention(std::span<const uint8_t> nal)
{
    std::vector<uint8_t> payload;
    payload.reserve(nal.size());
    int zeros = 0;
    for (uint8_t byte : nal) {
        if (zeros >= 2 && byte == EMULATION_PREVENTION) {
            zeros = 0;
            continue;
        }
        zeros = byte == 0 ? zeros + 1 : 0;
        payload.push_back(byte);
    }
    return payload;
}

std::vector<uint8_t> withEmulationPrevention(std::span<const uint8_t> payload)
{
    std::vector<uint8_t> nal;
    nal.reserve(payload.size() + payload.size() / 2 + 1);
    int zeros = 0;
    for (uint8_t byte : payload) {
        if (zeros >= 2 && byte <= EMULATION_PREVENTION) {
            nal.push_back(EMULATION_PREVENTION);
            zeros = 0;
        }
        zeros = byte == 0 ? zeros + 1 : 0;
        nal.push_back(byte);
    }
    // a zero at the very end would run into the next start code
    if (zeros > 0)
        nal.push_back(EMULATION_PREVENTION);
    return nal;
}

std::optional<SpsReordering> spsReordering(std::span<const uint8_t> sps)
{
    const std::optional<ParsedSps> parsed = parse(sps);
    return parsed ? std::optional(parsed->reordering) : std::nullopt;
}

std::optional<std::vector<uint8_t>> rewrittenSps(std::span<const uint8_t> sps)
{
    const std::optional<ParsedSps> parsed = parse(sps);
    if (!parsed || parsed->reordering.isKeptByOfficialClient())
        return std::nullopt;

    BitWriter writer;
    BitReader kept(parsed->unescaped);
    for (size_t left = parsed->keptBits; left > 0;) {
        const int count = static_cast<int>(std::min<size_t>(left, 32));
        writer.put(kept.bits(count), count);
        left -= static_cast<size_t>(count);
    }
    if (!parsed->reordering.hasVui) {
        writer.flag(true);
        writer.put(0, VUI_FLAGS_BEFORE_RESTRICTION);
    }
    if (!parsed->reordering.hasBitstreamRestriction) {
        writer.flag(true);
        writer.flag(ADDED_MOTION_VECTORS_OVER_PIC_BOUNDARIES);
        writer.ue(ADDED_MAX_BYTES_PER_PIC_DENOM);
        writer.ue(ADDED_MAX_BITS_PER_MB_DENOM);
        writer.ue(ADDED_LOG2_MAX_MV_LENGTH);
        writer.ue(ADDED_LOG2_MAX_MV_LENGTH);
    }
    const uint32_t maxDecFrameBuffering = parsed->reordering.maxNumRefFrames;
    writer.ue(NO_REORDERING);
    writer.ue(maxDecFrameBuffering);
    return withEmulationPrevention(writer.withTrailingBits());
}

bool rewriteSpsIn(std::vector<uint8_t> &accessUnit)
{
    const std::span<const uint8_t> data(accessUnit);
    std::vector<uint8_t> rewritten;
    size_t copied = 0;
    for (const std::span<const uint8_t> nal : Discord::Voice::splitAnnexB(data)) {
        const std::optional<std::vector<uint8_t>> sps = rewrittenSps(nal);
        if (!sps)
            continue;
        const size_t begin = static_cast<size_t>(nal.data() - data.data());
        rewritten.insert(rewritten.end(), data.begin() + copied, data.begin() + begin);
        rewritten.insert(rewritten.end(), sps->begin(), sps->end());
        copied = begin + nal.size();
    }
    if (rewritten.empty())
        return false;
    rewritten.insert(rewritten.end(), data.begin() + copied, data.end());
    accessUnit = std::move(rewritten);
    return true;
}

} // namespace H264
} // namespace Media
} // namespace Core
} // namespace Acheron
