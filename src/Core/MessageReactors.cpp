#include "MessageReactors.hpp"

#include <QPointer>

#include <utility>

#include "Discord/Client.hpp"
#include "UserManager.hpp"

namespace Acheron {
namespace Core {

bool ReactionRef::sameReaction(const ReactionRef &other) const
{
    return messageId == other.messageId && isBurst == other.isBurst && emoji.sameEmoji(other.emoji);
}

MessageReactors::MessageReactors(Discord::Client *client, UserManager *users, QObject *parent)
    : QObject(parent), client(client), users(users)
{
    connect(client, &Discord::Client::ready, this, &MessageReactors::onReady);
}

void MessageReactors::onReady()
{
    knownByReaction.clear();
}

MessageReactors::Key MessageReactors::keyOf(const ReactionRef &reaction)
{
    if (reaction.emoji.isUnicode())
        return { reaction.messageId, Snowflake::Invalid, reaction.emoji.name.get(), reaction.isBurst };
    return { reaction.messageId, reaction.emoji.id.get(), QString(), reaction.isBurst };
}

QList<Discord::User> MessageReactors::reactors(Snowflake channelId, const ReactionRef &reaction, int firstReadLimit)
{
    Known &known = knownByReaction[keyOf(reaction)];
    const bool firstRead = !std::exchange(known.askedApi, true);

    QList<Discord::User> resolved;
    for (Snowflake userId : std::as_const(known.reactorIds)) {
        if (const auto user = users->getUser(userId))
            resolved.append(*user);
    }

    if (firstRead)
        fetch(channelId, reaction, firstReadLimit, std::nullopt, {});
    return resolved;
}

void MessageReactors::fetchPage(Snowflake channelId, const ReactionRef &reaction, std::optional<Snowflake> after, PageFetched onFetched)
{
    fetch(channelId, reaction, PageSize, after, std::move(onFetched));
}

void MessageReactors::fetch(Snowflake channelId, const ReactionRef &reaction, int limit, std::optional<Snowflake> after, PageFetched onFetched)
{
    const QPointer<MessageReactors> self(this);
    client->fetchReactors(channelId, reaction.messageId, reaction.emoji.reactionKey(), reaction.isBurst, limit, after, [self, reaction, onFetched](const Result<QList<Discord::User>> &page) {
        if (!self)
            return;
        if (!page.success()) {
            if (onFetched)
                onFetched(Result<QList<Snowflake>>::makeError(page.error, page.code));
            return;
        }

        self->users->saveUsers(*page.value);
        Known &known = self->knownByReaction[keyOf(reaction)];
        QList<Snowflake> pageReactorIds;
        for (const Discord::User &user : *page.value) {
            pageReactorIds.append(user.id.get());
            if (!known.reactorIds.contains(user.id.get()))
                known.reactorIds.append(user.id.get());
        }
        emit self->reactorsChanged(reaction.messageId);

        if (onFetched)
            onFetched(Result<QList<Snowflake>>::makeOk(pageReactorIds));
    });
}

void MessageReactors::add(const ReactionRef &reaction, Snowflake userId)
{
    Known &known = knownByReaction[keyOf(reaction)];
    if (known.reactorIds.contains(userId))
        return;
    known.reactorIds.append(userId);
    emit reactorsChanged(reaction.messageId);
}

void MessageReactors::remove(const ReactionRef &reaction, Snowflake userId)
{
    const auto known = knownByReaction.find(keyOf(reaction));
    if (known != knownByReaction.end() && known->reactorIds.removeAll(userId) > 0)
        emit reactorsChanged(reaction.messageId);
}

void MessageReactors::forgetReactors(Snowflake messageId)
{
    for (auto it = knownByReaction.begin(); it != knownByReaction.end(); ++it) {
        if (it.key().messageId == messageId)
            it->reactorIds.clear();
    }
    emit reactorsChanged(messageId);
}

void MessageReactors::forgetReactors(Snowflake messageId, const Discord::Emoji &emoji)
{
    for (const bool isBurst : { false, true }) {
        const auto known = knownByReaction.find(keyOf({ messageId, emoji, isBurst }));
        if (known != knownByReaction.end())
            known->reactorIds.clear();
    }
    emit reactorsChanged(messageId);
}

} // namespace Core
} // namespace Acheron
