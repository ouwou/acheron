#include "RtpPacket.hpp"

#include <QtEndian>

namespace Acheron {
namespace Discord {
namespace AV {

QByteArray RtpHeader::serialize() const
{
    QByteArray data(FIXED_SIZE, '\0');
    auto *p = reinterpret_cast<uint8_t *>(data.data());

    p[0] = static_cast<uint8_t>((version << 6) | (padding ? 0x20 : 0) | (extension ? 0x10 : 0) | (csrcCount & 0x0F));
    p[1] = static_cast<uint8_t>((marker ? 0x80 : 0) | (payloadType & 0x7F));
    qToBigEndian(sequence, p + 2);
    qToBigEndian(timestamp, p + 4);
    qToBigEndian(ssrc, p + 8);

    return data;
}

RtpHeader RtpHeader::parse(const QByteArray &data)
{
    RtpHeader h;
    if (data.size() < FIXED_SIZE)
        return h;

    const auto *p = reinterpret_cast<const uint8_t *>(data.constData());

    h.version = (p[0] >> 6) & 0x03;
    h.padding = (p[0] >> 5) & 1;
    h.extension = (p[0] >> 4) & 1;
    h.csrcCount = p[0] & 0x0F;
    h.marker = (p[1] >> 7) & 1;
    h.payloadType = p[1] & 0x7F;
    h.sequence = qFromBigEndian<uint16_t>(p + 2);
    h.timestamp = qFromBigEndian<uint32_t>(p + 4);
    h.ssrc = qFromBigEndian<uint32_t>(p + 8);

    return h;
}

int rtpHeaderSize(const QByteArray &packet)
{
    if (packet.size() < RtpHeader::FIXED_SIZE)
        return -1;

    const auto *p = reinterpret_cast<const uint8_t *>(packet.constData());
    int csrcCount = p[0] & 0x0F;
    int size = RtpHeader::FIXED_SIZE + csrcCount * 4;

    bool hasExtension = (p[0] >> 4) & 1;
    if (hasExtension) {
        if (packet.size() < size + 4)
            return -1;
        uint16_t extLength = qFromBigEndian<uint16_t>(
                reinterpret_cast<const uint8_t *>(packet.constData()) + size + 2);
        size += 4 + extLength * 4;
    }

    if (packet.size() < size)
        return -1;

    return size;
}

} // namespace AV
} // namespace Discord
} // namespace Acheron
