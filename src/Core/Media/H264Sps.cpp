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

constexpr uint32_t MOST_PICTURES_IN_ORDER_CYCLE = 255;
constexpr uint32_t MOST_CODED_PICTURE_BUFFERS = 32;

// what the official client writes into a restriction it adds
constexpr uint32_t ADDED_MAX_BYTES_PER_PIC_DENOM = 2;
constexpr uint32_t ADDED_MAX_BITS_PER_MB_DENOM = 1;
constexpr uint32_t ADDED_LOG2_MAX_MV_LENGTH = 16;

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

    void ue(uint32_t value)
    {
        const uint64_t code = uint64_t(value) + 1;
        const int length = std::bit_width(code);
        put(0, length - 1);
        put(code, length);
    }

    [[nodiscard]] const std::vector<uint8_t> &aligned() const { return bytes; }

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
    const uint32_t codedPictureBuffers = reader.ue() + 1; // cpb_cnt_minus1
    if (codedPictureBuffers > MOST_CODED_PICTURE_BUFFERS)
        return reader.fail();
    reader.skip(8); // bit_rate_scale, cpb_size_scale
    for (uint32_t i = 0; i < codedPictureBuffers; i++) {
        reader.ue(); // bit_rate_value_minus1
        reader.ue(); // cpb_size_value_minus1
        reader.skip(1); // cbr_flag
    }
    reader.skip(20); // the lengths of the three delays and of the time offset
}

struct ParsedSps
{
    std::vector<uint8_t> unescaped;
    SpsReordering reordering;
    size_t keptBits = 0;
};

// sections 7.3.2.1.1 and E.1.1; skipped fields are named as in the standard
std::optional<ParsedSps> parse(std::span<const uint8_t> sps)
{
    if (sps.empty() || Discord::Voice::h264Type(sps[0]) != Discord::Voice::H264_SPS)
        return std::nullopt;

    ParsedSps parsed;
    parsed.unescaped = withoutEmulationPrevention(sps);
    BitReader reader(parsed.unescaped);
    SpsReordering &reordering = parsed.reordering;

    reader.skip(8); // the NAL unit's header
    const uint32_t profile = reader.bits(8);
    reader.skip(16); // constraint_set flags, level_idc
    reader.ue(); // seq_parameter_set_id
    if (std::ranges::find(PROFILES_WITH_CHROMA_FORMAT, profile) != PROFILES_WITH_CHROMA_FORMAT.end()) {
        const uint32_t chromaFormat = reader.ue();
        if (chromaFormat == 3)
            reader.skip(1); // separate_colour_plane_flag
        reader.ue(); // bit_depth_luma_minus8
        reader.ue(); // bit_depth_chroma_minus8
        reader.skip(1); // qpprime_y_zero_transform_bypass_flag
        if (reader.flag()) {
            const int scalingLists = chromaFormat == 3 ? 12 : 8;
            for (int i = 0; i < scalingLists; i++) {
                if (reader.flag())
                    skipScalingList(reader, i < 6 ? 16 : 64);
            }
        }
    }
    reader.ue(); // log2_max_frame_num_minus4
    const uint32_t pictureOrderCountType = reader.ue();
    if (pictureOrderCountType == 0) {
        reader.ue(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (pictureOrderCountType == 1) {
        reader.skip(1); // delta_pic_order_always_zero_flag
        reader.se(); // offset_for_non_ref_pic
        reader.se(); // offset_for_top_to_bottom_field
        const uint32_t picturesInCycle = reader.ue();
        if (picturesInCycle > MOST_PICTURES_IN_ORDER_CYCLE)
            return std::nullopt;
        for (uint32_t i = 0; i < picturesInCycle; i++)
            reader.se(); // offset_for_ref_frame
    }
    reordering.maxNumRefFrames = reader.ue();
    reader.skip(1); // gaps_in_frame_num_value_allowed_flag
    reader.ue(); // pic_width_in_mbs_minus1
    reader.ue(); // pic_height_in_map_units_minus1
    if (!reader.flag())
        reader.skip(1); // mb_adaptive_frame_field_flag
    reader.skip(1); // direct_8x8_inference_flag
    if (reader.flag()) {
        for (int i = 0; i < 4; i++)
            reader.ue(); // the four frame_crop offsets
    }

    parsed.keptBits = reader.position();
    reordering.hasVui = reader.flag();
    if (reordering.hasVui) {
        if (reader.flag() && reader.bits(8) == 255) // aspect_ratio_idc, of which 255 is Extended_SAR
            reader.skip(32); // sar_width, sar_height
        if (reader.flag())
            reader.skip(1); // overscan_appropriate_flag
        if (reader.flag()) {
            reader.skip(4); // video_format, video_full_range_flag
            if (reader.flag())
                reader.skip(24); // colour_primaries, transfer_characteristics, matrix_coefficients
        }
        if (reader.flag()) {
            reader.ue(); // chroma_sample_loc_type_top_field
            reader.ue(); // chroma_sample_loc_type_bottom_field
        }
        if (reader.flag())
            reader.skip(65); // num_units_in_tick, time_scale, fixed_frame_rate_flag
        const bool hasNalHrd = reader.flag();
        if (hasNalHrd)
            skipHrdParameters(reader);
        const bool hasVclHrd = reader.flag();
        if (hasVclHrd)
            skipHrdParameters(reader);
        if (hasNalHrd || hasVclHrd)
            reader.skip(1); // low_delay_hrd_flag
        reader.skip(1); // pic_struct_present_flag

        parsed.keptBits = reader.position();
        reordering.hasBitstreamRestriction = reader.flag();
        if (reordering.hasBitstreamRestriction) {
            reader.skip(1); // motion_vectors_over_pic_boundaries_flag
            reader.ue(); // max_bytes_per_pic_denom
            reader.ue(); // max_bits_per_mb_denom
            reader.ue(); // log2_max_mv_length_horizontal
            reader.ue(); // log2_max_mv_length_vertical
            parsed.keptBits = reader.position();
            reordering.maxNumReorderFrames = reader.ue();
            reordering.maxDecFrameBuffering = reader.ue();
        }
    }

    // a truncated SPS has no stop bit
    if (!reader.flag() || reader.failed())
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
        writer.put(1, 1);
        writer.put(0, 8);
    }
    if (!parsed->reordering.hasBitstreamRestriction) {
        writer.put(1, 1);
        writer.put(1, 1); // motion_vectors_over_pic_boundaries_flag
        writer.ue(ADDED_MAX_BYTES_PER_PIC_DENOM);
        writer.ue(ADDED_MAX_BITS_PER_MB_DENOM);
        writer.ue(ADDED_LOG2_MAX_MV_LENGTH);
        writer.ue(ADDED_LOG2_MAX_MV_LENGTH);
    }
    writer.ue(0); // max_num_reorder_frames
    writer.ue(parsed->reordering.maxNumRefFrames); // max_dec_frame_buffering
    writer.put(1, 1); // rbsp_stop_one_bit
    return withEmulationPrevention(writer.aligned());
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
