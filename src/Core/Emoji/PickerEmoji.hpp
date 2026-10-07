#pragma once

#include <QList>
#include <QString>
#include <QUrl>

#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

namespace Acheron {
namespace Core {

struct PickerEmoji
{
    QString name;
    QString surrogates;
    Snowflake customId;
    QString customName;
    Snowflake guildId;
    bool animated = false;

    [[nodiscard]] bool isCustom() const { return customId.isValid(); }

    [[nodiscard]] bool sameEmoji(const PickerEmoji &other) const
    {
        return isCustom() ? customId == other.customId : (!other.isCustom() && surrogates == other.surrogates);
    }

    [[nodiscard]] Discord::Emoji toReactionEmoji() const
    {
        return isCustom() ? Discord::Emoji::custom(customId, customName, animated) : Discord::Emoji::unicode(surrogates);
    }
};

struct PickerSection
{
    static constexpr auto RecentId = "recent";

    QString id;
    QString title;
    Snowflake guildId;
    QUrl guildIconUrl;
    QList<PickerEmoji> emojis;
};

} // namespace Core
} // namespace Acheron
