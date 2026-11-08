#include "ChannelPins.hpp"

#include <QPointer>

#include <algorithm>

#include "Discord/Client.hpp"
#include "Discord/Events.hpp"

namespace Acheron {
namespace Core {

ChannelPins::ChannelPins(Discord::Client *client, PrepareMessage prepare, QObject *parent)
    : QObject(parent), client(client), prepare(std::move(prepare))
{
    connect(client, &Discord::Client::ready, this, &ChannelPins::onReady);
    connect(client, &Discord::Client::channelDeleted, this, &ChannelPins::onChannelDeleted);
    connect(client, &Discord::Client::threadDeleted, this, &ChannelPins::onThreadDeleted);
    connect(client, &Discord::Client::guildDeleted, this, &ChannelPins::onGuildDeleted);
}

const ChannelPins::Channel *ChannelPins::channel(Snowflake channelId) const
{
    const auto it = channels.constFind(channelId);
    return it == channels.constEnd() ? nullptr : &it.value();
}

void ChannelPins::fetchFirstPage(Snowflake channelId)
{
    if (const Channel *known = channel(channelId)) {
        if (!known->newestFirst.isEmpty() || known->state == LoadState::Loading || known->state == LoadState::Finished)
            return;
    }
    fetch(channelId, std::nullopt);
}

void ChannelPins::fetchPageAfterOldest(Snowflake channelId)
{
    const Channel *known = channel(channelId);
    if (!known || known->newestFirst.isEmpty())
        return;
    if (known->state != LoadState::HasMore && known->state != LoadState::Failed)
        return;
    fetch(channelId, known->newestFirst.last().pinnedAt);
}

void ChannelPins::fetch(Snowflake channelId, std::optional<QDateTime> pinnedBefore)
{
    auto it = channels.find(channelId);
    if (it == channels.end())
        it = channels.insert(channelId, Channel{ client->getGuildIdForChannel(channelId) });
    it->state = LoadState::Loading;
    emit loadStateChanged(channelId);

    const QPointer<ChannelPins> self(this);
    client->fetchMessagePins(channelId, PageSize, pinnedBefore, [self, channelId](const Result<Discord::Client::MessagePinsPage> &page) {
        if (!self)
            return;
        auto it = self->channels.find(channelId);
        if (it == self->channels.end() || it->state != LoadState::Loading)
            return;

        if (!page.success()) {
            it->state = LoadState::Failed;
            emit self->loadStateChanged(channelId);
            return;
        }

        for (const Discord::MessagePin &pin : page.value->newestFirst) {
            const Snowflake messageId = pin.message->id.get();
            const bool alreadyKnown = std::any_of(it->newestFirst.cbegin(), it->newestFirst.cend(), [messageId](const Pinned &p) { return p.message.id.get() == messageId; });
            if (alreadyKnown)
                continue;
            Pinned pinned{ pin.pinnedAt.get(), pin.message.get() };
            self->prepare(pinned.message);
            it->newestFirst.append(pinned);
        }
        it->state = page.value->hasMore ? LoadState::HasMore : LoadState::Finished;
        emit self->pinsChanged(channelId);
        emit self->loadStateChanged(channelId);
    });
}

void ChannelPins::pin(Snowflake channelId, Snowflake messageId)
{
    const QPointer<ChannelPins> self(this);
    client->pinMessage(channelId, messageId, [self, channelId](const Result<void> &result) {
        if (self && !result.success())
            emit self->pinFailed(channelId, result.code, result.error);
    });
}

void ChannelPins::unpin(Snowflake channelId, Snowflake messageId)
{
    const QPointer<ChannelPins> self(this);
    client->unpinMessage(channelId, messageId, [self, channelId, messageId](const Result<void> &result) {
        if (self && !result.success())
            emit self->unpinFailed(channelId, messageId);
    });
}

void ChannelPins::applyMessageUpdate(const Discord::Message &update)
{
    const Snowflake channelId = update.channelId.get();
    auto it = channels.find(channelId);
    if (it == channels.end())
        return;

    const Snowflake messageId = update.id.get();
    auto known = std::find_if(it->newestFirst.begin(), it->newestFirst.end(), [messageId](const Pinned &p) { return p.message.id.get() == messageId; });

    if (update.presentKeys.contains(QStringLiteral("pinned")) && !update.isPinned()) {
        if (known == it->newestFirst.end())
            return;
        it->newestFirst.erase(known);
    } else if (known != it->newestFirst.end()) {
        known->message.applyUpdate(update);
        prepare(known->message);
    } else if (update.isPinned() && update.presentKeys.contains(QStringLiteral("author"))) {
        // the official client files a newly pinned message under the time it saw the update
        Pinned pinned{ QDateTime::currentDateTimeUtc(), update };
        prepare(pinned.message);
        it->newestFirst.prepend(pinned);
    } else {
        return;
    }
    emit pinsChanged(channelId);
}

void ChannelPins::forgetMessage(Snowflake channelId, Snowflake messageId)
{
    auto it = channels.find(channelId);
    if (it == channels.end())
        return;
    auto &pins = it->newestFirst;
    const auto removed = std::remove_if(pins.begin(), pins.end(), [messageId](const Pinned &p) { return p.message.id.get() == messageId; });
    if (removed == pins.end())
        return;
    pins.erase(removed, pins.end());
    emit pinsChanged(channelId);
}

void ChannelPins::onReady()
{
    channels.clear();
}

void ChannelPins::forgetChannel(Snowflake channelId)
{
    if (channels.remove(channelId))
        emit pinsChanged(channelId);
}

void ChannelPins::onChannelDeleted(const Discord::ChannelDelete &event)
{
    forgetChannel(event.id.get());
}

void ChannelPins::onThreadDeleted(const Discord::ThreadDelete &event)
{
    forgetChannel(event.id.get());
}

void ChannelPins::onGuildDeleted(const Discord::GuildDelete &event)
{
    const Snowflake guildId = event.id.get();
    QList<Snowflake> gone;
    for (auto it = channels.cbegin(); it != channels.cend(); ++it)
        if (it->guildId == guildId)
            gone.append(it.key());
    for (Snowflake channelId : gone)
        forgetChannel(channelId);
}

} // namespace Core
} // namespace Acheron
