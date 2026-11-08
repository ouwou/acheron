#pragma once

#include "Discord/Voice/PacketBuffer.hpp"

#include <QByteArray>

#include <cstdint>
#include <span>

namespace PacketBytes {

inline std::span<const uint8_t> bytesOf(const QByteArray &bytes)
{
    return { reinterpret_cast<const uint8_t *>(bytes.constData()), static_cast<size_t>(bytes.size()) };
}

inline QByteArray toByteArray(std::span<const uint8_t> bytes)
{
    return QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()));
}

inline QByteArray contentsOf(const Acheron::Discord::Voice::PacketBuffer &packet)
{
    return toByteArray(packet.bytes());
}

} // namespace PacketBytes
