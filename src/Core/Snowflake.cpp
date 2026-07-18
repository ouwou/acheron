#include "Snowflake.hpp"

#include <QDateTime>
#include <QRandomGenerator>

namespace Acheron {
namespace Core {

constexpr quint64 DISCORD_EPOCH = 1420070400000ULL;

const Snowflake Snowflake::Invalid = -1ULL;

Snowflake::Snowflake() : value(Invalid) {}

Snowflake::Snowflake(quint64 n) : value(n) {}

bool Snowflake::isValid() const noexcept
{
    return *this != Invalid;
}

QString Snowflake::toString() const noexcept
{
    return QString::number(value);
}

Snowflake::operator quint64() const noexcept
{
    return value;
}

QDateTime Snowflake::toDateTime() const noexcept
{
    if (!isValid())
        return {};
    const qint64 ms = static_cast<qint64>((value >> 22) + DISCORD_EPOCH);
    return QDateTime::fromMSecsSinceEpoch(ms);
}

Snowflake Snowflake::generateNonce()
{
    qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    quint64 nonce = static_cast<quint64>(timestamp - DISCORD_EPOCH) << 22;
    // discord does nothing to the lower 22 bits
    return Snowflake(nonce);
}

Snowflake Snowflake::fromUnixMs(qint64 ms)
{
    if (ms <= static_cast<qint64>(DISCORD_EPOCH))
        return Snowflake(0);
    return Snowflake(static_cast<quint64>(ms - DISCORD_EPOCH) << 22);
}

} // namespace Core
} // namespace Acheron