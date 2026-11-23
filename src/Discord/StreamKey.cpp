#include "StreamKey.hpp"

#include <QStringList>

namespace Acheron {
namespace Discord {

namespace {

const QLatin1String GUILD_TYPE("guild");
const QLatin1String CALL_TYPE("call");

std::optional<Core::Snowflake> parseId(const QString &text)
{
    bool isNumber = false;
    const quint64 value = text.toULongLong(&isNumber);
    if (!isNumber || value == 0)
        return std::nullopt;
    return Core::Snowflake(value);
}

} // namespace

StreamKey StreamKey::forChannel(Core::Snowflake guildId, Core::Snowflake channelId, Core::Snowflake ownerId)
{
    StreamKey key;
    key.type = guildId.isValid() ? Type::Guild : Type::Call;
    key.guildId = guildId;
    key.channelId = channelId;
    key.ownerId = ownerId;
    return key;
}

std::optional<StreamKey> StreamKey::parse(const QString &key)
{
    const QStringList parts = key.split(QLatin1Char(':'));
    if (parts.isEmpty())
        return std::nullopt;

    StreamKey parsed;
    int idIndex = 1;
    if (parts.first() == GUILD_TYPE && parts.size() == 4) {
        parsed.type = Type::Guild;
        const auto guildId = parseId(parts.at(idIndex++));
        if (!guildId)
            return std::nullopt;
        parsed.guildId = *guildId;
    } else if (parts.first() == CALL_TYPE && parts.size() == 3) {
        parsed.type = Type::Call;
    } else {
        return std::nullopt;
    }

    const auto channelId = parseId(parts.at(idIndex++));
    const auto ownerId = parseId(parts.at(idIndex));
    if (!channelId || !ownerId)
        return std::nullopt;
    parsed.channelId = *channelId;
    parsed.ownerId = *ownerId;

    return parsed;
}

QString StreamKey::toString() const
{
    if (type == Type::Guild)
        return QStringLiteral("guild:%1:%2:%3").arg(quint64(guildId)).arg(quint64(channelId)).arg(quint64(ownerId));
    return QStringLiteral("call:%1:%2").arg(quint64(channelId)).arg(quint64(ownerId));
}

QString StreamKey::typeName() const
{
    return type == Type::Guild ? GUILD_TYPE : CALL_TYPE;
}

} // namespace Discord
} // namespace Acheron
