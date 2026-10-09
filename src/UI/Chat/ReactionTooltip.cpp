#include "ReactionTooltip.hpp"

#include <QCoreApplication>
#include <QStringList>

#include "Core/ClientInstance.hpp"
#include "Core/Emoji/UnicodeEmojiIndex.hpp"

namespace Acheron {
namespace UI {

static QString translated(const char *text)
{
    return QCoreApplication::translate("Acheron::UI::ReactionTooltip", text);
}

static QString namesAndOthers(QStringList names, int others)
{
    if (others == 1)
        names.append(translated("1 other"));
    else if (others > 1)
        names.append(translated("%1 others").arg(others));

    if (names.size() == 1)
        return names.first();
    if (names.size() == 2)
        return translated("%1 and %2").arg(names.first(), names.last());

    const QString last = names.takeLast();
    return translated("%1, and %2").arg(names.join(QStringLiteral(", ")), last);
}

QString reactionEmojiName(const Discord::Emoji &emoji)
{
    if (!emoji.isUnicode())
        return ":" + emoji.name.get() + ":";

    const Core::UnicodeEmoji *known = Core::UnicodeEmojiIndex::instance().bySurrogate(emoji.name.get());
    return known ? ":" + Core::UnicodeEmojiIndex::primaryName(*known) + ":" : emoji.name.get();
}

QString reactionTooltip(Core::ClientInstance *instance, Core::Snowflake channelId, Core::Snowflake guildId, const Core::ReactionRef &reaction, int reactionCount)
{
    QStringList names;
    for (const Discord::User &reactor : instance->messages()->reactors()->reactors(channelId, reaction, Core::MessageReactors::TooltipSample)) {
        if (instance->relationships()->isBlockedOrIgnored(reactor.id.get()))
            continue;
        names.append(instance->users()->getAuthorDisplayName(reactor, guildId));
        if (names.size() == Core::MessageReactors::TooltipSample)
            break;
    }
    if (names.isEmpty())
        return {};

    const int others = qMax(0, reactionCount - int(names.size()));
    const QString reactedBy = reaction.isBurst ? translated("%1 Super reacted by %2") : translated("%1 reacted by %2");
    return reactedBy.arg(reactionEmojiName(reaction.emoji), namesAndOthers(names, others));
}

} // namespace UI
} // namespace Acheron
