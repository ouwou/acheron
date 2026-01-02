#include "MessageManager.hpp"

#include <QtConcurrent>

#include "Discord/Client.hpp"
#include "Markdown/Parser.hpp"

namespace Acheron {
namespace Core {

MessageManager::MessageManager(Snowflake accountId, Discord::Client *client, QObject *parent)
    : QObject(parent), client(client), repo(accountId)
{
    messageCache.setMaxCost(1'000);

    // connect(client, &Discord::Client::messagesReceived, this, &MessageManager::onApiMessagesReceived);
    // connect(client, &Discord::Client::messagesFailed, this, &MessageManager::onMessagesFailed);
}

MessageManager::~MessageManager() { }

void MessageManager::requestLoadChannel(Snowflake channelId)
{
    if (fetchedChannels.contains(channelId)) {
        // ram cache
        if (channelMessages.contains(channelId)) {
            const auto &order = channelMessages[channelId];

            int count = order.size();
            int startIndex = (count > 50) ? (count - 50) : 0;

            bool cached = true;
            QList<Discord::Message> result;
            result.reserve(count - startIndex);

            for (int i = startIndex; i < count; i++) {
                Snowflake msgId = order[i];
                if (auto *msg = messageCache.object(msgId)) {
                    result.append(*msg);
                } else {
                    cached = false;
                    break;
                }
            }

            if (cached) {
                emit messagesReceived({
                        true,
                        Discord::Client::MessageLoadType::Latest,
                        channelId,
                        result,
                });
                return;
            }
        }

        // disk cache
        QList<Discord::Message> msgs = repo.getLatestMessages(channelId, 50);
        if (!msgs.isEmpty()) { // probably good
            emit messagesReceived({
                    true,
                    Discord::Client::MessageLoadType::Latest,
                    channelId,
                    msgs,
            });
            return;
        }
    }

    QPointer<MessageManager> guard = this;
    client->fetchLatestMessages(
            channelId, 50, [this, guard, channelId](const Result<QList<Discord::Message>> &result) {
                if (!guard)
                    return;

                if (!result.success()) {
                    qWarning() << "Failed to fetch messages" << result.error;
                } else {
                    onApiMessagesReceived(result.value.value(),
                                          Discord::Client::MessageLoadType::Latest, channelId);
                }
            });
}

void MessageManager::requestLoadHistory(Snowflake channelId, Snowflake beforeId)
{
    if (historyDebounce.contains(channelId))
        return;

    if (lowestKnownId.contains(channelId)) {
        // nothing to see here
        if (lowestKnownId[channelId] >= beforeId) {
            emit messagesReceived({
                    true,
                    Discord::Client::MessageLoadType::History,
                    channelId,
                    {},
            });
            return;
        }
    }

    if (fetchedChannels.contains(channelId)) {
        // ram cache
        const auto &order = channelMessages[channelId];
        if (!order.empty()) {
            auto it = std::lower_bound(order.begin(), order.end(), beforeId);
            int index = std::distance(order.begin(), it);

            if (index > 0) {
                int count = std::min(index, 50);
                int startIndex = index - count;

                bool cached = true;
                QList<Discord::Message> result;
                result.reserve(count);

                for (int i = startIndex; i < index; i++) {
                    Snowflake msgId = order[i];
                    if (auto *msg = messageCache.object(msgId)) {
                        result.append(*msg);
                    } else {
                        cached = false;
                        break;
                    }
                }

                if (cached) {
                    emit messagesReceived({
                            true,
                            Discord::Client::MessageLoadType::History,
                            channelId,
                            result,
                    });
                    return;
                }
            }
        }

        // disk cache
        QList<Discord::Message> msgs = repo.getMessagesBefore(channelId, beforeId, 50);
        if (!msgs.isEmpty()) { // probably good
            emit messagesReceived({
                    true,
                    Discord::Client::MessageLoadType::History,
                    channelId,
                    msgs,
            });
            return;
        }
    }

    historyDebounce.insert(channelId);

    QPointer<MessageManager> guard = this;
    client->fetchHistory(channelId, beforeId, 50,
                         [this, guard, channelId](const Result<QList<Discord::Message>> &result) {
                             if (!guard)
                                 return;

                             historyDebounce.remove(channelId);

                             if (!result.success()) {
                                 qWarning() << "Failed to fetch history" << result.error;
                                 emit messagesReceived({
                                         false,
                                         Discord::Client::MessageLoadType::History,
                                 });
                             } else {
                                 onApiMessagesReceived(result.value.value(),
                                                       Discord::Client::MessageLoadType::History,
                                                       channelId);
                             }
                         });
}

void MessageManager::cacheMessages(Snowflake channelId, const QList<Discord::Message> &msgs)
{
    if (msgs.isEmpty())
        return;

    auto &order = channelMessages[channelId];
    for (const auto &msg : msgs) {
        messageCache.insert(msg.id, new Discord::Message(msg));
        auto it = std::lower_bound(order.begin(), order.end(), msg.id);
        if (it == order.end() || *it != msg.id.get())
            order.insert(it, msg.id);
    }
}

void MessageManager::onMessageCreated(const Discord::Message &message)
{
    onApiMessagesReceived({ message }, Discord::Client::MessageLoadType::Created,
                          message.channelId);
}

void MessageManager::onApiMessagesReceived(const QList<Discord::Message> &messages,
                                           Discord::Client::MessageLoadType type,
                                           Snowflake channelId)
{
    repo.saveMessages(messages);

    auto sortedMessages = messages;
    std::sort(sortedMessages.begin(), sortedMessages.end(),
              [](const auto &a, const auto &b) { return a.id.get() < b.id.get(); });

    if (type == Discord::Client::MessageLoadType::Latest ||
        type == Discord::Client::MessageLoadType::History) {
        if (sortedMessages.size() < 50) // hit the end probably
            lowestKnownId[channelId] = sortedMessages.first().id;
    }

    for (auto &msg : sortedMessages) {
        static Markdown::Parser parser;
        auto ast = parser.parse(msg.content);
        msg.parsedContentCached = parser.toHtml(ast);
    }

    for (const auto &msg : sortedMessages) {
        // cache owns its own copy
        Discord::Message *toCache = new Discord::Message(msg);
        messageCache.insert(toCache->id, toCache); // should remove old copies
    }

    if (type == Discord::Client::MessageLoadType::Latest) {
        fetchedChannels.insert(channelId);
        // channel load means channelMessages should be fresh
        channelMessages[channelId].clear();
    }

    cacheMessages(channelId, sortedMessages);

    emit messagesReceived({ true, type, channelId, sortedMessages });
}

} // namespace Core
} // namespace Acheron
