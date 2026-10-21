#include "VideoDepacketizer.hpp"

#include "Av1Obu.hpp"
#include "NalUnits.hpp"
#include "RtpVideoFormat.hpp"

#include <QStringList>

#include <algorithm>
#include <array>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

using Payload = std::span<const uint8_t>;

void appendStartCode(std::vector<uint8_t> &frame)
{
    frame.insert(frame.end(), START_CODE.begin(), START_CODE.end());
}

// first_mb_in_slice 0 starts a picture; ue(v) codes 0 as a single 1 bit
bool beginsPicture(Payload sliceData)
{
    return !sliceData.empty() && (sliceData[0] & 0x80);
}

bool h264StartsKeyframe(Payload payload)
{
    if (payload.empty())
        return false;

    const uint8_t type = payload[0] & H264_TYPE_MASK;
    if (type == H264_SPS)
        return true;
    if (type == H264_IDR)
        return beginsPicture(payload.subspan(1));
    if (type == H264_STAP_A) {
        Payload units = payload.subspan(1);
        while (units.size() > 2) {
            const size_t size = (size_t(units[0]) << 8) | units[1];
            if (size == 0 || size > units.size() - 2)
                return false;
            const uint8_t unitType = units[2] & H264_TYPE_MASK;
            if (unitType == H264_SPS || (unitType == H264_IDR && beginsPicture(units.subspan(3, size - 1))))
                return true;
            units = units.subspan(2 + size);
        }
        return false;
    }
    if (type == H264_FU_A)
        return payload.size() > 2 && (payload[1] & FU_START) && (payload[1] & H264_TYPE_MASK) == H264_IDR && beginsPicture(payload.subspan(2));
    return false;
}

bool appendAggregatedNals(Payload units, std::vector<uint8_t> &frame)
{
    while (!units.empty()) {
        if (units.size() < 2)
            return false;
        const size_t size = (size_t(units[0]) << 8) | units[1];
        units = units.subspan(2);
        if (size == 0 || size > units.size())
            return false;
        appendStartCode(frame);
        appendBytes(frame, units.first(size));
        units = units.subspan(size);
    }
    return true;
}

bool assembleH264(std::span<const Payload> payloads, std::vector<uint8_t> &frame)
{
    bool inFragment = false;
    for (Payload payload : payloads) {
        if (payload.empty())
            return false;

        const uint8_t type = payload[0] & H264_TYPE_MASK;
        if (type == H264_FU_A) {
            if (payload.size() < 3)
                return false;
            if (payload[1] & FU_START) {
                appendStartCode(frame);
                frame.push_back(static_cast<uint8_t>((payload[0] & 0xE0) | (payload[1] & H264_TYPE_MASK)));
                inFragment = true;
            } else if (!inFragment) {
                return false;
            }
            appendBytes(frame, payload.subspan(2));
            continue;
        }

        inFragment = false;
        if (type == H264_STAP_A) {
            if (!appendAggregatedNals(payload.subspan(1), frame))
                return false;
        } else if (type >= 1 && type <= 23) {
            appendStartCode(frame);
            appendBytes(frame, payload);
        } else {
            return false;
        }
    }
    return !frame.empty();
}

bool h265StartsKeyframe(Payload payload)
{
    if (payload.size() < 3)
        return false;

    const uint8_t type = h265Type(payload[0]);
    if (type == H265_VPS || type == H265_SPS)
        return true;
    // first_slice_segment_in_pic_flag is the slice header's first bit
    if (h265IsIrap(type))
        return payload[2] & 0x80;
    if (type == H265_AP) {
        Payload units = payload.subspan(2);
        while (units.size() > 4) {
            const size_t size = (size_t(units[0]) << 8) | units[1];
            if (size < 3 || size > units.size() - 2)
                return false;
            const uint8_t unitType = h265Type(units[2]);
            if (unitType == H265_VPS || unitType == H265_SPS || (h265IsIrap(unitType) && (units[4] & 0x80)))
                return true;
            units = units.subspan(2 + size);
        }
        return false;
    }
    if (type == H265_FU)
        return payload.size() > 3 && (payload[2] & FU_START) && h265IsIrap(payload[2] & 0x3F) && (payload[3] & 0x80);
    return false;
}

bool assembleH265(std::span<const Payload> payloads, std::vector<uint8_t> &frame)
{
    bool inFragment = false;
    for (Payload payload : payloads) {
        if (payload.size() < 3)
            return false;

        const uint8_t type = h265Type(payload[0]);
        if (type == H265_FU) {
            if (payload[2] & FU_START) {
                appendStartCode(frame);
                frame.push_back(static_cast<uint8_t>((payload[0] & 0x81) | ((payload[2] & 0x3F) << 1)));
                frame.push_back(payload[1]);
                inFragment = true;
            } else if (!inFragment) {
                return false;
            }
            appendBytes(frame, payload.subspan(3));
            continue;
        }

        inFragment = false;
        if (type == H265_AP) {
            if (!appendAggregatedNals(payload.subspan(2), frame))
                return false;
        } else if (type < H265_AP) {
            appendStartCode(frame);
            appendBytes(frame, payload);
        } else {
            return false;
        }
    }
    return !frame.empty();
}

// RFC 7741
bool stripVp8Descriptor(Payload payload, Payload &data, bool &opensFrame)
{
    if (payload.empty())
        return false;

    const uint8_t first = payload[0];
    size_t offset = 1;
    if (first & VP8_EXTENDED) {
        if (payload.size() <= offset)
            return false;
        const uint8_t extension = payload[offset++];
        if (extension & VP8_HAS_PICTURE_ID) {
            if (payload.size() <= offset)
                return false;
            offset += (payload[offset] & VP8_LONG_PICTURE_ID) ? 2 : 1;
        }
        if (extension & VP8_HAS_TL0_PICTURE_INDEX)
            offset += 1;
        if (extension & VP8_HAS_TEMPORAL_ID_OR_KEY_INDEX)
            offset += 1;
    }
    if (payload.size() <= offset)
        return false;

    opensFrame = (first & VP8_PARTITION_START) && (first & VP8_PARTITION_ID_MASK) == 0;
    data = payload.subspan(offset);
    return true;
}

bool vp8StartsKeyframe(Payload payload)
{
    Payload data;
    bool opensFrame = false;
    if (!stripVp8Descriptor(payload, data, opensFrame))
        return false;
    // VP8: bit 0 of the first byte is clear on a key frame
    return opensFrame && !(data[0] & 0x01);
}

bool assembleVp8(std::span<const Payload> payloads, std::vector<uint8_t> &frame)
{
    bool first = true;
    for (Payload payload : payloads) {
        Payload data;
        bool opensFrame = false;
        if (!stripVp8Descriptor(payload, data, opensFrame))
            return false;
        if (first && !opensFrame)
            return false;
        first = false;
        appendBytes(frame, data);
    }
    return !frame.empty();
}

using namespace Av1;

bool av1StartsKeyframe(Payload payload)
{
    return !payload.empty() && !(payload[0] & AV1_CONTINUES_PREVIOUS) && (payload[0] & AV1_NEW_SEQUENCE);
}

bool assembleAv1(std::span<const Payload> payloads, std::vector<uint8_t> &frame)
{
    std::vector<std::vector<uint8_t>> obus;
    bool openObu = false;

    for (size_t i = 0; i < payloads.size(); i++) {
        Payload payload = payloads[i];
        if (payload.empty())
            return false;

        const uint8_t aggregation = payload[0];
        const int elementCount = (aggregation >> AV1_ELEMENT_COUNT_SHIFT) & AV1_ELEMENT_COUNT_MASK;
        const bool continuesPrevious = aggregation & AV1_CONTINUES_PREVIOUS;
        if (continuesPrevious != openObu || (i == 0 && continuesPrevious))
            return false;
        payload = payload.subspan(1);

        for (int index = 0; !payload.empty(); index++) {
            Payload element = payload;
            if (elementCount == 0 || index + 1 < elementCount) {
                size_t size = 0;
                if (!readLeb128(payload, size) || size > payload.size())
                    return false;
                element = payload.first(size);
                payload = payload.subspan(size);
            } else {
                payload = {};
            }

            if (index == 0 && openObu) {
                if (obus.empty())
                    return false;
                obus.back().insert(obus.back().end(), element.begin(), element.end());
            } else {
                obus.emplace_back(element.begin(), element.end());
            }
        }

        openObu = aggregation & AV1_CONTINUES_IN_NEXT;
    }

    if (openObu || obus.empty())
        return false;

    for (size_t i = 0; i < obus.size(); i++) {
        Payload bytes = obus[i];
        const std::optional<ParsedObu> obu = readObu(bytes);
        if (!obu)
            return false;

        const bool last = i + 1 == obus.size();
        frame.push_back(static_cast<uint8_t>(last ? (obu->header[0] & ~OBU_HAS_SIZE) : (obu->header[0] | OBU_HAS_SIZE)));
        appendBytes(frame, obu->header.subspan(1));
        if (!last)
            appendLeb128(frame, obu->body.size());
        appendBytes(frame, obu->body);
    }
    return true;
}

void sizeLastObu(std::vector<uint8_t> &frame)
{
    for (Payload rest(frame); !rest.empty();) {
        const size_t offset = frame.size() - rest.size();
        const std::optional<ParsedObu> obu = readObu(rest);
        if (!obu)
            return;
        if (obu->hasSize)
            continue;

        std::vector<uint8_t> size;
        appendLeb128(size, obu->body.size());
        frame[offset] |= OBU_HAS_SIZE;
        frame.insert(frame.begin() + static_cast<std::ptrdiff_t>(offset + obu->header.size()), size.begin(), size.end());
        return;
    }
}

// DAVE's trailer: an 8-byte tag, the nonce, the clear ranges as (offset, size) pairs, the trailer's
// own size, and 0xFAFA
QString describeEncryptionTrailer(Payload frame)
{
    constexpr size_t TAG_SIZE = 8;
    if (frame.size() < TAG_SIZE + 4 || frame[frame.size() - 1] != 0xFA || frame[frame.size() - 2] != 0xFA)
        return QStringLiteral("not encrypted");

    const size_t trailerSize = frame[frame.size() - 3];
    if (trailerSize < TAG_SIZE + 4 || trailerSize > frame.size())
        return QStringLiteral("encrypted, trailer size %1 invalid").arg(trailerSize);

    Payload fields = frame.subspan(frame.size() - trailerSize + TAG_SIZE, trailerSize - TAG_SIZE - 3);
    size_t nonce = 0;
    if (!readLeb128(fields, nonce))
        return QStringLiteral("encrypted, nonce unreadable");

    QStringList ranges;
    while (!fields.empty()) {
        size_t offset = 0;
        size_t size = 0;
        if (!readLeb128(fields, offset) || !readLeb128(fields, size))
            break;
        ranges.append(QStringLiteral("%1+%2").arg(offset).arg(size));
    }
    return QStringLiteral("encrypted, trailer %1 bytes, nonce %2, clear ranges [%3]").arg(trailerSize).arg(nonce).arg(ranges.join(QLatin1Char(' ')));
}

QString describeNals(Payload frame, bool h265)
{
    constexpr int MAX_LISTED = 16;

    QStringList units;
    for (Payload nal : splitAnnexB(frame)) {
        if (units.size() == MAX_LISTED)
            break;
        const int type = h265 ? h265Type(nal[0]) : h264Type(nal[0]);
        units.append(QStringLiteral("%1@%2").arg(type).arg(nal.data() - frame.data()));
    }
    return units.join(QLatin1Char(' '));
}

} // namespace

QString describeFrame(VideoCodec codec, std::span<const uint8_t> frame)
{
    QString units;
    if (codec == VideoCodec::H264 || codec == VideoCodec::H265)
        units = QStringLiteral(", units (type@offset) %1").arg(describeNals(frame, codec == VideoCodec::H265));
    return QStringLiteral("%1 frame of %2 bytes%3; %4").arg(videoCodecName(codec)).arg(frame.size()).arg(units, describeEncryptionTrailer(frame));
}

bool startsKeyframe(VideoCodec codec, std::span<const uint8_t> payload)
{
    switch (codec) {
    case VideoCodec::H264:
        return h264StartsKeyframe(payload);
    case VideoCodec::H265:
        return h265StartsKeyframe(payload);
    case VideoCodec::VP8:
        return vp8StartsKeyframe(payload);
    case VideoCodec::AV1:
        return av1StartsKeyframe(payload);
    case VideoCodec::Unknown:
        break;
    }
    return false;
}

bool startsWithParameterSets(VideoCodec codec, std::span<const uint8_t> payload)
{
    switch (codec) {
    case VideoCodec::H264: {
        if (payload.empty())
            return false;
        const uint8_t type = payload[0] & H264_TYPE_MASK;
        return type == H264_SPS || (type == H264_STAP_A && h264StartsKeyframe(payload));
    }
    case VideoCodec::H265: {
        if (payload.size() < 3)
            return false;
        const uint8_t type = h265Type(payload[0]);
        return type == H265_VPS || type == H265_SPS || (type == H265_AP && h265StartsKeyframe(payload));
    }
    case VideoCodec::VP8:
    case VideoCodec::AV1:
        return startsKeyframe(codec, payload);
    case VideoCodec::Unknown:
        break;
    }
    return false;
}

bool assembleFrame(VideoCodec codec, std::span<const std::span<const uint8_t>> payloads, std::vector<uint8_t> &frame)
{
    frame.clear();
    switch (codec) {
    case VideoCodec::H264:
        return assembleH264(payloads, frame);
    case VideoCodec::H265:
        return assembleH265(payloads, frame);
    case VideoCodec::VP8:
        return assembleVp8(payloads, frame);
    case VideoCodec::AV1:
        return assembleAv1(payloads, frame);
    case VideoCodec::Unknown:
        break;
    }
    return false;
}

void prepareForDecoder(VideoCodec codec, std::vector<uint8_t> &frame)
{
    if (codec == VideoCodec::AV1)
        sizeLastObu(frame);
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
