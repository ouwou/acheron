#include "MessageManager.hpp"

#include <QtConcurrent>

#include "Discord/Client.hpp"
#include "Markdown/Parser.hpp"
#include "Logging.hpp"
#include "UserManager.hpp"

namespace Acheron {
namespace Core {

MessageManager::MessageManager(Snowflake accountId, Discord::Client *client,
                               UserManager *userManager, QObject *parent)
    : QObject(parent), client(client), userManager(userManager), repo(accountId), parser(std::make_unique<Markdown::Parser>())
{
    messageCache.setMaxCost(1'000);

    parser->setUserResolver([this](const QString &userId) {
        Snowflake id(userId.toULongLong());
        return this->userManager->getDisplayName(id);
    });

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
            int startIndex = (count > 30) ? (count - 30) : 0;

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
        QList<Discord::Message> msgs = repo.getLatestMessages(channelId, 30);
        if (!msgs.isEmpty()) { // probably good
            for (auto &msg : msgs) {
                Markdown::ParseState state;
                state.isInline = true;
                auto ast = parser->parse(msg.content, state);
                bool jumbo = Markdown::Parser::isEmojiOnly(ast);
                msg.parsedContentCached = parser->toHtml(ast, jumbo);
            }

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
            channelId, 30, [this, guard, channelId](const Result<QList<Discord::Message>> &result) {
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
                int count = std::min(index, 30);
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
        QList<Discord::Message> msgs = repo.getMessagesBefore(channelId, beforeId, 30);

        for (auto &msg : msgs) {
            Markdown::ParseState state;
            state.isInline = true;
            auto ast = parser->parse(msg.content, state);
            bool jumbo = Markdown::Parser::isEmojiOnly(ast);
            msg.parsedContentCached = parser->toHtml(ast, jumbo);
        }

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
    client->fetchHistory(channelId, beforeId, 30,
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

void MessageManager::onMessageUpdated(const Discord::Message &message)
{
    Discord::Message updatedMsg = message;
    Markdown::ParseState state;
    state.isInline = true;
    auto ast = parser->parse(updatedMsg.content, state);
    bool jumbo = Markdown::Parser::isEmojiOnly(ast);
    updatedMsg.parsedContentCached = parser->toHtml(ast, jumbo);

    messageCache.insert(message.id, new Discord::Message(updatedMsg));

    repo.saveMessages({ updatedMsg });

    emit messagesReceived({ true, Discord::Client::MessageLoadType::Created, message.channelId, { updatedMsg } });
}

void MessageManager::onMessageDeleted(const Discord::MessageDelete &event)
{
    Snowflake channelId = event.channelId.get();
    Snowflake messageId = event.id.get();

    messageCache.remove(messageId);

    if (channelMessages.contains(channelId)) {
        auto &order = channelMessages[channelId];
        auto it = std::find(order.begin(), order.end(), messageId);
        if (it != order.end())
            order.erase(it);
    }

    repo.markMessageDeleted(messageId);

    emit messageDeleted(channelId, messageId);
}

void MessageManager::onMessageSendFailed(const QString &nonce, const QString &error)
{
    qCWarning(LogCore) << "Message send failed for nonce" << nonce << ":" << error;

    emit messageErrored(nonce);
}

void MessageManager::sendMessage(Snowflake channelId, const QString &content,
                                 Snowflake replyToMessageId)
{
    Snowflake nonceId = Snowflake::generateNonce();
    QString nonce = QString::number(nonceId);

    Discord::Message preview;
    preview.id = nonceId; // temporary id, will be overwritten
    preview.nonce = nonce;
    preview.channelId = channelId;
    preview.content = content;
    preview.timestamp = QDateTime::currentDateTimeUtc();
    preview.author = client->getMe();
    preview.flags = Discord::MessageFlags(0);
    preview.isPendingOutbound = true;

    if (replyToMessageId.isValid()) {
        preview.type = Discord::MessageType::REPLY;
        Discord::MessageReference ref;
        ref.messageId = replyToMessageId;
        ref.channelId = channelId;
        preview.messageReference = ref;
    } else {
        preview.type = Discord::MessageType::DEFAULT;
    }

    Markdown::ParseState state;
    state.isInline = true;
    auto ast = parser->parse(content, state);
    bool jumbo = Markdown::Parser::isEmojiOnly(ast);
    preview.parsedContentCached = parser->toHtml(ast, jumbo);

    // get our fake preview in
    emit messagesReceived(
            { true, Discord::Client::MessageLoadType::Created, channelId, { preview } });

    client->sendMessage(channelId, content, nonce, replyToMessageId);
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
        // hit the end probably
        // maybe 0 could happen mistakenly prob not tho
        if (sortedMessages.size() == 0)
            lowestKnownId[channelId] = 0;
        else if (sortedMessages.size() < 30)
            lowestKnownId[channelId] = sortedMessages.first().id;
    }

    for (auto &msg : sortedMessages) {
        Markdown::ParseState state;
        state.isInline = true;
        auto ast = parser->parse(msg.content, state);
        bool jumbo = Markdown::Parser::isEmojiOnly(ast);
        msg.parsedContentCached = parser->toHtml(ast, jumbo);
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
