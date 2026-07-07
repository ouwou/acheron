#pragma once

#include <QString>
#include <QUrl>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Discord {
namespace Cdn {

inline QString assetExtension(const QString &hash)
{
    return hash.startsWith(QStringLiteral("a_")) ? QStringLiteral("gif") : QStringLiteral("png");
}

inline QUrl defaultUserAvatar(Core::Snowflake userId, int size = 128)
{
    if (!userId.isValid())
        return {};
    const quint64 index = (quint64(userId) >> 22) % 6;
    return QUrl(QStringLiteral("https://cdn.discordapp.com/embed/avatars/%1.png?size=%2")
                        .arg(QString::number(index), QString::number(size)));
}

inline QUrl userAvatar(Core::Snowflake userId, const QString &hash, int size = 128)
{
    if (hash.isEmpty())
        return defaultUserAvatar(userId, size);
    return QUrl(QStringLiteral("https://cdn.discordapp.com/avatars/%1/%2.%3?size=%4")
                        .arg(QString::number(quint64(userId)), hash, assetExtension(hash),
                             QString::number(size)));
}

inline QUrl guildMemberAvatar(Core::Snowflake guildId, Core::Snowflake userId,
                              const QString &hash, int size = 128)
{
    if (hash.isEmpty())
        return {};
    return QUrl(QStringLiteral(
                        "https://cdn.discordapp.com/guilds/%1/users/%2/avatars/%3.%4?size=%5")
                        .arg(QString::number(quint64(guildId)),
                             QString::number(quint64(userId)), hash, assetExtension(hash),
                             QString::number(size)));
}

inline QUrl userBanner(Core::Snowflake userId, const QString &hash, int size = 600)
{
    if (hash.isEmpty())
        return {};
    return QUrl(QStringLiteral("https://cdn.discordapp.com/banners/%1/%2.%3?size=%4")
                        .arg(QString::number(quint64(userId)), hash, assetExtension(hash),
                             QString::number(size)));
}

inline QUrl guildIcon(Core::Snowflake guildId, const QString &hash, int size = 64)
{
    if (hash.isEmpty())
        return {};
    return QUrl(QStringLiteral("https://cdn.discordapp.com/icons/%1/%2.%3?size=%4")
                        .arg(QString::number(quint64(guildId)), hash, assetExtension(hash),
                             QString::number(size)));
}

inline QUrl channelIcon(Core::Snowflake channelId, const QString &hash, int size = 64)
{
    if (hash.isEmpty())
        return {};
    return QUrl(QStringLiteral("https://cdn.discordapp.com/channel-icons/%1/%2.png?size=%3")
                        .arg(QString::number(quint64(channelId)), hash, QString::number(size)));
}

inline QUrl badgeIcon(const QString &hash, int size = 64)
{
    if (hash.isEmpty())
        return {};
    return QUrl(QStringLiteral("https://cdn.discordapp.com/badge-icons/%1.png?size=%2")
                        .arg(hash, QString::number(size)));
}

QUrl connectionIcon(const QString &type);

} // namespace Cdn
} // namespace Discord
} // namespace Acheron
