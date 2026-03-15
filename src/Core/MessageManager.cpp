#include "MessageManager.hpp"

#include <QtConcurrent>
#include <QJsonArray>
#include <QJsonDocument>

#include "Discord/Client.hpp"
#include "Markdown/Parser.hpp"
#include "Logging.hpp"
#include "UserManager.hpp"

namespace Acheron {
namespace Core {

static QString resolveUserJoinMessage(const Discord::Message &msg)
{
    QString author = msg.author->getDisplayName();
    qint64 ms = msg.timestamp->toMSecsSinceEpoch();
    switch (ms % 13) {
    case 0:
        return author + QStringLiteral(" joined the party.");
    case 1:
        return author + QStringLiteral(" is here.");
    case 2:
        return QStringLiteral("Welcome, ") + author + QStringLiteral(". We hope you brought pizza.");
    case 3:
        return QStringLiteral("A wild ") + author + QStringLiteral(" appeared.");
    case 4:
        return author + QStringLiteral(" just landed.");
    case 5:
        return author + QStringLiteral(" just slid into the server.");
    case 6:
        return author + QStringLiteral(" just showed up!");
    case 7:
        return QStringLiteral("Welcome ") + author + QStringLiteral(". Say hi!");
    case 8:
        return author + QStringLiteral(" hopped into the server.");
    case 9:
        return QStringLiteral("Everyone welcome ") + author + QStringLiteral("!");
    case 10:
        return QStringLiteral("Glad you're here, ") + author + QStringLiteral(".");
    case 11:
        return QStringLiteral("Good to see you, ") + author + QStringLiteral(".");
    case 12:
        return QStringLiteral("Yay you made it, ") + author + QStringLiteral("!");
    default:
        return author + QStringLiteral(" joined the party.");
    }
}

static QString resolveSystemMessageContent(const Discord::Message &msg)
{
    QString author = msg.author->getDisplayName();
    switch (static_cast<Discord::MessageType>(msg.type.get())) {
    case Discord::MessageType::CALL:
        return author + QStringLiteral(" started a call.");
    case Discord::MessageType::USER_JOIN:
        return resolveUserJoinMessage(msg);
    default:
        return msg.content;
    }
}

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
                auto ast = parser->parse(resolveSystemMessageContent(msg), state);
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
            auto ast = parser->parse(resolveSystemMessageContent(msg), state);
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
    auto ast = parser->parse(resolveSystemMessageContent(updatedMsg), state);
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

static bool emojisMatch(const Discord::Emoji &a, const Discord::Emoji &b)
{
    if (!a.isUnicode() && !b.isUnicode())
        return a.id.get() == b.id.get();
    if (a.isUnicode() && b.isUnicode())
        return a.name.get() == b.name.get();
    return false;
}

// todo i dont like this here
static QString reactionsToJson(const QList<Discord::Reaction> &reactions)
{
    if (reactions.isEmpty())
        return {};

    QJsonArray arr;
    for (const auto &r : reactions) {
        QJsonObject emojiObj;
        if (!r.emoji->isUnicode())
            emojiObj["id"] = r.emoji->id->toString();
        else
            emojiObj["id"] = QJsonValue::Null;
        emojiObj["name"] = *r.emoji->name;
        if (r.emoji->animated.hasValue())
            emojiObj["animated"] = *r.emoji->animated;

        QJsonObject obj;
        obj["emoji"] = emojiObj;
        obj["count"] = *r.count;
        obj["me"] = *r.me;

        if (r.countDetails.hasValue()) {
            QJsonObject details;
            details["burst"] = *r.countDetails->burst;
            details["normal"] = *r.countDetails->normal;
            obj["count_details"] = details;
        }

        if (r.meBurst.hasValue())
            obj["me_burst"] = *r.meBurst;
        if (r.burstCount.hasValue())
            obj["burst_count"] = *r.burstCount;

        if (r.burstColors.hasValue()) {
            QJsonArray colors;
            for (const auto &c : *r.burstColors)
                colors.append(c);
            obj["burst_colors"] = colors;
        }

        arr.append(obj);
    }

    QJsonDocument doc(arr);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

static void rebuildReactionsJson(Discord::Message &msg)
{
    if (!msg.reactions.hasValue()) {
        msg.reactionsJson.clear();
        return;
    }
    msg.reactionsJson = reactionsToJson(*msg.reactions);
}

static QList<Discord::Reaction> reactionsFromJson(const QString &json)
{
    QList<Discord::Reaction> reactions;
    if (json.isEmpty())
        return reactions;

    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isArray()) {
        for (const QJsonValue &val : doc.array())
            reactions.append(Discord::Reaction::fromJson(val.toObject()));
    }
    return reactions;
}

void MessageManager::emitReactionUpdate(Discord::Message &msg)
{
    rebuildReactionsJson(msg);
    messageCache.insert(msg.id, new Discord::Message(msg));
    repo.saveMessages({ msg });
    emit messagesReceived({ true, Discord::Client::MessageLoadType::Created, msg.channelId, { msg } });
}

static void applyReactionAdd(QList<Discord::Reaction> &reactions,
                             const Discord::Emoji &emoji, bool isBurst, bool isMe,
                             const QList<QString> &burstColors = {})
{
    bool found = false;
    for (auto &r : reactions) {
        if (emojisMatch(r.emoji, emoji)) {
            r.count = *r.count + 1;
            if (r.countDetails.hasValue()) {
                if (isBurst)
                    r.countDetails->burst = *r.countDetails->burst + 1;
                else
                    r.countDetails->normal = *r.countDetails->normal + 1;
            }
            if (isMe) {
                if (isBurst)
                    r.meBurst = true;
                else
                    r.me = true;
            }
            if (isBurst && !burstColors.isEmpty())
                r.burstColors = burstColors;
            found = true;
            break;
        }
    }

    if (!found) {
        Discord::Reaction newReaction;
        newReaction.emoji = emoji;
        newReaction.count = 1;
        newReaction.me = !isBurst && isMe;
        newReaction.meBurst = isBurst && isMe;
        newReaction.burstCount = isBurst ? 1 : 0;

        Discord::ReactionCountDetails details;
        details.burst = isBurst ? 1 : 0;
        details.normal = isBurst ? 0 : 1;
        newReaction.countDetails = details;

        if (isBurst && !burstColors.isEmpty())
            newReaction.burstColors = burstColors;

        reactions.append(newReaction);
    }
}

void MessageManager::onReactionAdd(const Discord::MessageReactionAdd &event)
{
    bool isBurst = event.type.hasValue() && *event.type == 1;
    bool isMe = event.userId.get() == client->getMe().id.get();

    auto *cached = messageCache.object(event.messageId);
    if (!cached) {
        QList<Discord::Reaction> reactions =
                reactionsFromJson(repo.getReactionsJson(event.messageId));
        QList<QString> colors = event.burstColors.hasValue() ? *event.burstColors : QList<QString>{};
        applyReactionAdd(reactions, event.emoji, isBurst, isMe, colors);
        repo.updateReactionsJson(event.messageId, reactionsToJson(reactions));
        return;
    }

    Discord::Message msg = *cached;

    if (!msg.reactions.hasValue())
        msg.reactions = QList<Discord::Reaction>();

    QList<QString> colors = event.burstColors.hasValue() ? *event.burstColors : QList<QString>{};
    applyReactionAdd(*msg.reactions, event.emoji, isBurst, isMe, colors);

    emitReactionUpdate(msg);
}

static void applyReactionAddMany(QList<Discord::Reaction> &reactions,
                                 const Discord::MessageReactionAddMany &event, Snowflake myId)
{
    for (const auto &debounced : *event.reactions) {
        bool isMe = false;
        for (const auto &uid : *debounced.users) {
            if (uid == myId) {
                isMe = true;
                break;
            }
        }

        int addCount = debounced.users->size();
        bool found = false;
        for (auto &r : reactions) {
            if (emojisMatch(r.emoji, debounced.emoji)) {
                r.count = *r.count + addCount;
                if (r.countDetails.hasValue())
                    r.countDetails->normal = *r.countDetails->normal + addCount;
                if (isMe)
                    r.me = true;
                found = true;
                break;
            }
        }

        if (!found) {
            Discord::Reaction newReaction;
            newReaction.emoji = debounced.emoji;
            newReaction.count = addCount;
            newReaction.me = isMe;
            newReaction.meBurst = false;
            newReaction.burstCount = 0;

            Discord::ReactionCountDetails details;
            details.burst = 0;
            details.normal = addCount;
            newReaction.countDetails = details;

            reactions.append(newReaction);
        }
    }
}

void MessageManager::onReactionAddMany(const Discord::MessageReactionAddMany &event)
{
    Snowflake myId = client->getMe().id;

    auto *cached = messageCache.object(event.messageId);
    if (!cached) {
        QList<Discord::Reaction> reactions =
                reactionsFromJson(repo.getReactionsJson(event.messageId));
        applyReactionAddMany(reactions, event, myId);
        repo.updateReactionsJson(event.messageId, reactionsToJson(reactions));
        return;
    }

    Discord::Message msg = *cached;

    if (!msg.reactions.hasValue())
        msg.reactions = QList<Discord::Reaction>();

    applyReactionAddMany(*msg.reactions, event, myId);

    emitReactionUpdate(msg);
}

static void applyReactionRemove(QList<Discord::Reaction> &reactions,
                                const Discord::Emoji &emoji, bool isBurst, bool isMe)
{
    for (int i = 0; i < reactions.size(); ++i) {
        auto &r = reactions[i];
        if (emojisMatch(r.emoji, emoji)) {
            r.count = *r.count - 1;
            if (r.countDetails.hasValue()) {
                if (isBurst)
                    r.countDetails->burst = qMax(0, *r.countDetails->burst - 1);
                else
                    r.countDetails->normal = qMax(0, *r.countDetails->normal - 1);
            }
            if (isMe) {
                if (isBurst)
                    r.meBurst = false;
                else
                    r.me = false;
            }

            if (*r.count <= 0)
                reactions.removeAt(i);

            break;
        }
    }
}

void MessageManager::onReactionRemove(const Discord::MessageReactionRemove &event)
{
    bool isBurst = event.type.hasValue() && *event.type == 1;
    bool isMe = event.userId.get() == client->getMe().id.get();

    auto *cached = messageCache.object(event.messageId);
    if (!cached) {
        QList<Discord::Reaction> reactions =
                reactionsFromJson(repo.getReactionsJson(event.messageId));
        applyReactionRemove(reactions, event.emoji, isBurst, isMe);
        repo.updateReactionsJson(event.messageId, reactionsToJson(reactions));
        return;
    }

    Discord::Message msg = *cached;

    if (!msg.reactions.hasValue())
        return;

    applyReactionRemove(*msg.reactions, event.emoji, isBurst, isMe);

    emitReactionUpdate(msg);
}

void MessageManager::onReactionRemoveAll(const Discord::MessageReactionRemoveAll &event)
{
    auto *cached = messageCache.object(event.messageId);
    if (!cached) {
        repo.updateReactionsJson(event.messageId, {});
        return;
    }

    Discord::Message msg = *cached;
    msg.reactions = QList<Discord::Reaction>();

    emitReactionUpdate(msg);
}

void MessageManager::onReactionRemoveEmoji(const Discord::MessageReactionRemoveEmoji &event)
{
    auto *cached = messageCache.object(event.messageId);
    if (!cached) {
        QList<Discord::Reaction> reactions =
                reactionsFromJson(repo.getReactionsJson(event.messageId));
        for (int i = 0; i < reactions.size(); ++i) {
            if (emojisMatch(reactions[i].emoji, event.emoji)) {
                reactions.removeAt(i);
                break;
            }
        }
        repo.updateReactionsJson(event.messageId, reactionsToJson(reactions));
        return;
    }

    Discord::Message msg = *cached;

    if (!msg.reactions.hasValue())
        return;

    for (int i = 0; i < msg.reactions->size(); ++i) {
        if (emojisMatch((*msg.reactions)[i].emoji, event.emoji)) {
            msg.reactions->removeAt(i);
            break;
        }
    }

    emitReactionUpdate(msg);
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
        auto ast = parser->parse(resolveSystemMessageContent(msg), state);
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
