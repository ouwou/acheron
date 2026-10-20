#include "VideoDepacketizer.hpp"

#include <QStringList>

#include <algorithm>
#include <array>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

using Payload = std::span<const uint8_t>;

constexpr std::array<uint8_t, 4> START_CODE = { 0, 0, 0, 1 };

void appendStartCode(std::vector<uint8_t> &frame)
{
    frame.insert(frame.end(), START_CODE.begin(), START_CODE.end());
}

void append(std::vector<uint8_t> &frame, Payload bytes)
{
    frame.insert(frame.end(), bytes.begin(), bytes.end());
}

// RFC 6184
constexpr uint8_t H264_TYPE_MASK = 0x1F;
constexpr uint8_t H264_IDR = 5;
constexpr uint8_t H264_SPS = 7;
constexpr uint8_t H264_STAP_A = 24;
constexpr uint8_t H264_FU_A = 28;
constexpr uint8_t FU_START = 0x80;

// A slice whose first_mb_in_slice is 0 starts a picture; ue(v) codes 0 as a single 1 bit.
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

// Aggregation packets of both H264 and H265 are a run of 16-bit lengths, each followed by a NAL.
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
        append(frame, units.first(size));
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
            append(frame, payload.subspan(2));
            continue;
        }

        inFragment = false;
        if (type == H264_STAP_A) {
            if (!appendAggregatedNals(payload.subspan(1), frame))
                return false;
        } else if (type >= 1 && type <= 23) {
            appendStartCode(frame);
            append(frame, payload);
        } else {
            return false;
        }
    }
    return !frame.empty();
}

// RFC 7798
constexpr uint8_t H265_VPS = 32;
constexpr uint8_t H265_SPS = 33;
constexpr uint8_t H265_AP = 48;
constexpr uint8_t H265_FU = 49;
constexpr uint8_t H265_IRAP_FIRST = 16;
constexpr uint8_t H265_IRAP_LAST = 23;

uint8_t h265Type(uint8_t firstHeaderByte)
{
    return (firstHeaderByte >> 1) & 0x3F;
}

bool h265IsIrap(uint8_t type)
{
    return type >= H265_IRAP_FIRST && type <= H265_IRAP_LAST;
}

bool h265StartsKeyframe(Payload payload)
{
    if (payload.size() < 3)
        return false;

    const uint8_t type = h265Type(payload[0]);
    if (type == H265_VPS || type == H265_SPS)
        return true;
    // first_slice_segment_in_pic_flag is the first bit of the slice header.
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
            append(frame, payload.subspan(3));
            continue;
        }

        inFragment = false;
        if (type == H265_AP) {
            if (!appendAggregatedNals(payload.subspan(2), frame))
                return false;
        } else if (type < H265_AP) {
            appendStartCode(frame);
            append(frame, payload);
        } else {
            return false;
        }
    }
    return !frame.empty();
}

// RFC 7741. Returns the payload behind the descriptor, and whether this packet opens a frame.
bool stripVp8Descriptor(Payload payload, Payload &data, bool &opensFrame)
{
    if (payload.empty())
        return false;

    const uint8_t first = payload[0];
    size_t offset = 1;
    if (first & 0x80) {
        if (payload.size() <= offset)
            return false;
        const uint8_t extension = payload[offset++];
        if (extension & 0x80) {
            if (payload.size() <= offset)
                return false;
            offset += (payload[offset] & 0x80) ? 2 : 1;
        }
        if (extension & 0x40)
            offset += 1;
        if (extension & 0x30)
            offset += 1;
    }
    if (payload.size() <= offset)
        return false;

    opensFrame = (first & 0x10) && (first & 0x07) == 0;
    data = payload.subspan(offset);
    return true;
}

bool vp8StartsKeyframe(Payload payload)
{
    Payload data;
    bool opensFrame = false;
    if (!stripVp8Descriptor(payload, data, opensFrame))
        return false;
    // The frame tag's lowest bit is 0 on a key frame.
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
        append(frame, data);
    }
    return !frame.empty();
}

// https://aomediacodec.github.io/av1-rtp-spec/
constexpr uint8_t AV1_CONTINUES_PREVIOUS = 0x80;
constexpr uint8_t AV1_CONTINUES_IN_NEXT = 0x40;
constexpr uint8_t AV1_NEW_SEQUENCE = 0x08;
constexpr uint8_t OBU_HAS_EXTENSION = 0x04;
constexpr uint8_t OBU_HAS_SIZE = 0x02;
constexpr int LEB128_MAX_BYTES = 8;

bool readLeb128(Payload &bytes, size_t &value)
{
    value = 0;
    for (int i = 0; i < LEB128_MAX_BYTES; i++) {
        if (bytes.empty())
            return false;
        const uint8_t byte = bytes[0];
        bytes = bytes.subspan(1);
        value |= size_t(byte & 0x7F) << (7 * i);
        if (!(byte & 0x80))
            return true;
    }
    return false;
}

void appendLeb128(std::vector<uint8_t> &out, size_t value)
{
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value != 0)
            byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
}

bool av1StartsKeyframe(Payload payload)
{
    return !payload.empty() && !(payload[0] & AV1_CONTINUES_PREVIOUS) && (payload[0] & AV1_NEW_SEQUENCE);
}

// Splits an OBU as it travels in RTP into its header bytes and its payload. An element should
// not carry a size field, but one that does has it removed here.
bool splitObu(Payload obu, Payload &header, Payload &body)
{
    if (obu.empty())
        return false;
    const size_t headerSize = (obu[0] & OBU_HAS_EXTENSION) ? 2 : 1;
    if (obu.size() < headerSize)
        return false;

    header = obu.first(headerSize);
    body = obu.subspan(headerSize);
    if (obu[0] & OBU_HAS_SIZE) {
        size_t size = 0;
        if (!readLeb128(body, size) || size > body.size())
            return false;
        body = body.first(size);
    }
    return true;
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
        const int elementCount = (aggregation >> 4) & 0x03;
        const bool continuesPrevious = aggregation & AV1_CONTINUES_PREVIOUS;
        if (continuesPrevious != openObu || (i == 0 && continuesPrevious))
            return false;
        payload = payload.subspan(1);

        for (int index = 0; !payload.empty(); index++) {
            Payload element = payload;
            // With a count, every element but the last carries its length; without, all do.
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
        Payload header, body;
        if (!splitObu(obus[i], header, body))
            return false;

        const bool last = i + 1 == obus.size();
        frame.push_back(static_cast<uint8_t>(last ? (header[0] & ~OBU_HAS_SIZE) : (header[0] | OBU_HAS_SIZE)));
        append(frame, header.subspan(1));
        if (!last)
            appendLeb128(frame, body.size());
        append(frame, body);
    }
    return true;
}

void sizeLastObu(std::vector<uint8_t> &frame)
{
    size_t offset = 0;
    while (offset < frame.size()) {
        const uint8_t header = frame[offset];
        const size_t headerSize = (header & OBU_HAS_EXTENSION) ? 2 : 1;
        if (offset + headerSize > frame.size())
            return;

        if (!(header & OBU_HAS_SIZE)) {
            std::vector<uint8_t> size;
            appendLeb128(size, frame.size() - offset - headerSize);
            frame[offset] = header | OBU_HAS_SIZE;
            frame.insert(frame.begin() + static_cast<std::ptrdiff_t>(offset + headerSize), size.begin(), size.end());
            return;
        }

        Payload rest(frame.data() + offset + headerSize, frame.size() - offset - headerSize);
        const size_t before = rest.size();
        size_t size = 0;
        if (!readLeb128(rest, size) || size > rest.size())
            return;
        offset += headerSize + (before - rest.size()) + size;
    }
}

// The trailer the end-to-end encryptor appends: an 8-byte tag, the nonce, the ranges it left
// readable as (offset, size) pairs, the trailer's own size, and the marker 0xFAFA.
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
    size_t offset = 0;
    while (offset + START_CODE.size() < frame.size() && units.size() < MAX_LISTED) {
        if (!std::equal(START_CODE.begin(), START_CODE.end(), frame.begin() + static_cast<std::ptrdiff_t>(offset))) {
            offset++;
            continue;
        }
        const size_t header = offset + START_CODE.size();
        const int type = h265 ? h265Type(frame[header]) : (frame[header] & H264_TYPE_MASK);
        units.append(QStringLiteral("%1@%2").arg(type).arg(offset));
        offset = header;
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
        // An aggregate that reached startsKeyframe() by way of its parameter sets counts too.
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
        // These formats mark the first packet of a key frame itself.
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
