#include "RtpPacket.hpp"

#include <QtEndian>

#include <cstring>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

constexpr uint8_t RTP_VERSION = 2;
constexpr uint16_t ONE_BYTE_EXTENSION_PROFILE = 0xBEDE;
constexpr uint8_t EXTENSION_ID_PADDING = 0;
constexpr uint8_t EXTENSION_ID_RESERVED = 15;
constexpr int MAX_ONE_BYTE_EXTENSION_VALUE = 16;

// what discord_voice.node treats as RTCP; narrower than rtcp-mux's 192..223
constexpr uint8_t RTCP_SECOND_BYTE_FIRST = 194;
constexpr uint8_t RTCP_SECOND_BYTE_LAST = 223;

bool hasRtpVersion(std::span<const uint8_t> packet)
{
    return !packet.empty() && (packet[0] >> 6) == RTP_VERSION;
}

} // namespace

PacketKind classifyPacket(std::span<const uint8_t> packet)
{
    const int size = static_cast<int>(packet.size());
    if (size < RTCP_CLEAR_SIZE || !hasRtpVersion(packet))
        return PacketKind::Unknown;

    if (packet[1] >= RTCP_SECOND_BYTE_FIRST && packet[1] <= RTCP_SECOND_BYTE_LAST)
        return PacketKind::Rtcp;

    return size >= RtpHeader::FIXED_SIZE ? PacketKind::Rtp : PacketKind::Unknown;
}

std::optional<ParsedRtp> parseRtp(std::span<const uint8_t> packet)
{
    if (static_cast<int>(packet.size()) < RtpHeader::FIXED_SIZE || !hasRtpVersion(packet))
        return std::nullopt;

    const uint8_t *p = packet.data();

    ParsedRtp parsed;
    parsed.header.padding = (p[0] >> 5) & 1;
    parsed.header.extension = (p[0] >> 4) & 1;
    parsed.header.csrcCount = p[0] & 0x0F;
    parsed.header.marker = (p[1] >> 7) & 1;
    parsed.header.payloadType = p[1] & 0x7F;
    parsed.header.sequence = qFromBigEndian<uint16_t>(p + 2);
    parsed.header.timestamp = qFromBigEndian<uint32_t>(p + 4);
    parsed.header.ssrc = qFromBigEndian<uint32_t>(p + 8);

    parsed.clearSize = RtpHeader::FIXED_SIZE + parsed.header.csrcCount * 4;
    if (parsed.header.extension) {
        if (static_cast<int>(packet.size()) < parsed.clearSize + RtpHeader::EXTENSION_PREAMBLE_SIZE)
            return std::nullopt;
        parsed.extensionSize = qFromBigEndian<uint16_t>(p + parsed.clearSize + 2) * 4;
        parsed.clearSize += RtpHeader::EXTENSION_PREAMBLE_SIZE;
    }

    if (static_cast<int>(packet.size()) < parsed.clearSize)
        return std::nullopt;

    return parsed;
}

std::optional<int> rtpPaddingSize(std::span<const uint8_t> payload)
{
    if (payload.empty())
        return std::nullopt;
    const int padding = payload.back();
    if (padding == 0 || padding > static_cast<int>(payload.size()))
        return std::nullopt;
    return padding;
}

int writeRtpHeader(const RtpHeader &header, std::span<uint8_t> out, int extensionWords)
{
    const int size = RtpHeader::FIXED_SIZE + (header.extension ? RtpHeader::EXTENSION_PREAMBLE_SIZE : 0);
    if (header.csrcCount != 0 || static_cast<int>(out.size()) < size || extensionWords < 0 || extensionWords > 0xFFFF)
        return -1;

    uint8_t *p = out.data();
    p[0] = static_cast<uint8_t>((RTP_VERSION << 6) | (header.padding ? 0x20 : 0) | (header.extension ? 0x10 : 0));
    p[1] = static_cast<uint8_t>((header.marker ? 0x80 : 0) | (header.payloadType & 0x7F));
    qToBigEndian(header.sequence, p + 2);
    qToBigEndian(header.timestamp, p + 4);
    qToBigEndian(header.ssrc, p + 8);

    if (header.extension) {
        qToBigEndian(ONE_BYTE_EXTENSION_PROFILE, p + RtpHeader::FIXED_SIZE);
        qToBigEndian(static_cast<uint16_t>(extensionWords), p + RtpHeader::FIXED_SIZE + 2);
    }

    return size;
}

std::span<const uint8_t> findRtpExtension(std::span<const uint8_t> elements, uint8_t id)
{
    size_t offset = 0;
    while (offset < elements.size()) {
        const uint8_t elementId = elements[offset] >> 4;
        if (elementId == EXTENSION_ID_PADDING) {
            ++offset;
            continue;
        }
        if (elementId == EXTENSION_ID_RESERVED)
            break;

        const size_t length = (elements[offset] & 0x0F) + 1u;
        if (offset + 1 + length > elements.size())
            break;
        if (elementId == id)
            return elements.subspan(offset + 1, length);

        offset += 1 + length;
    }
    return {};
}

int writeRtpExtensions(std::span<const RtpExtension> elements, std::span<uint8_t> out)
{
    size_t size = 0;
    for (const RtpExtension &element : elements) {
        const size_t length = element.value.size();
        if (element.id == EXTENSION_ID_PADDING || element.id >= EXTENSION_ID_RESERVED || length == 0 || length > MAX_ONE_BYTE_EXTENSION_VALUE)
            return -1;
        if (size + 1 + length > out.size())
            return -1;

        out[size] = static_cast<uint8_t>((element.id << 4) | (length - 1));
        std::memcpy(out.data() + size + 1, element.value.data(), length);
        size += 1 + length;
    }

    const size_t padded = (size + 3) & ~size_t(3);
    if (padded > out.size())
        return -1;
    std::memset(out.data() + size, 0, padded - size);

    return static_cast<int>(padded);
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
