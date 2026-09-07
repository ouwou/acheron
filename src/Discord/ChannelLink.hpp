#pragma once

#include <optional>

#include <QRegularExpression>
#include <QString>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Discord {

// https://[canary.|ptb.]discord[app].com/channels/<guild or @me>/<channel>[/<message>]
struct ChannelLink
{
    Core::Snowflake guildId = Core::Snowflake::Invalid;
    Core::Snowflake channelId;
    Core::Snowflake messageId = Core::Snowflake::Invalid;

    static QRegularExpression pattern(const QString &suffix = {})
    {
        return QRegularExpression(
                R"(^https?:\/\/(?:(?:canary|ptb)\.)?discord(?:app)?\.com\/channels\/(@me|\d+)\/(\d+)(?:\/(\d+))?\/?)" + suffix,
                QRegularExpression::CaseInsensitiveOption);
    }

    static ChannelLink fromMatch(const QRegularExpressionMatch &match)
    {
        ChannelLink link;
        if (match.captured(1) != "@me")
            link.guildId = Core::Snowflake(match.captured(1).toULongLong());
        link.channelId = Core::Snowflake(match.captured(2).toULongLong());
        if (!match.captured(3).isEmpty())
            link.messageId = Core::Snowflake(match.captured(3).toULongLong());
        return link;
    }

    static std::optional<ChannelLink> parse(const QString &url)
    {
        static const QRegularExpression whole = pattern("$");
        auto match = whole.match(url);
        if (!match.hasMatch())
            return std::nullopt;
        return fromMatch(match);
    }
};

} // namespace Discord
} // namespace Acheron
