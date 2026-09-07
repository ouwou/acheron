#include "MessageManager.hpp"

#include <QDateTime>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

#include "Discord/Client.hpp"
#include "Emoji/EmojiManager.hpp"
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

MessageManager::~MessageManager() {}

void MessageManager::parseMessageContent(Discord::Message &msg)
{
    msg.parsedContentCached = inlineHtml(resolveSystemMessageContent(msg), msg.channelId);

    if (msg.type.hasValue() && msg.type.get() == Discord::MessageType::THREAD_STARTER_MESSAGE &&
        msg.referencedMessage && msg.referencedMessage->content.hasValue() &&
        msg.referencedMessage->parsedContentCached.isEmpty())
        msg.referencedMessage->parsedContentCached =
                inlineHtml(msg.referencedMessage->content.get(), msg.referencedMessage->channelId);

    if (msg.snapshotMessage && msg.snapshotMessage->content.hasValue() &&
        msg.snapshotMessage->parsedContentCached.isEmpty())
        msg.snapshotMessage->parsedContentCached = inlineHtml(msg.snapshotMessage->content.get(), msg.channelId);
}

static Markdown::ParseState inlineParseState(Snowflake channelId)
{
    Markdown::ParseState state;
    state.isInline = true;
    state.customState["channelId"] = quint64(channelId);
    return state;
}

QString MessageManager::inlineHtml(const QString &content, Snowflake channelId) const
{
    auto ast = parser->parse(content, inlineParseState(channelId));
    return parser->toHtml(ast, Markdown::Parser::isEmojiOnly(ast));
}

void MessageManager::setChannelLinkResolver(Markdown::ChannelLinkResolverFn resolver)
{
    parser->setChannelLinkResolver(std::move(resolver));
}

void MessageManager::setChannelResolver(std::function<QString(Snowflake)> resolver)
{
    parser->setChannelResolver([resolver](const QString &channelId) {
        Snowflake id(channelId.toULongLong());
        return resolver(id);
    });
}

void MessageManager::setEmojiManager(EmojiManager *manager)
{
    emojiManager = manager;
}

void MessageManager::requestLoadChannel(Snowflake channelId)
{
    if (pagesInFlight.contains({ channelId, LoadType::Latest }))
        return;

    if (fetchedChannels.contains(channelId)) {
        if (const auto *tail = segments[channelId].tail()) {
            int count = static_cast<int>(tail->ids.size());
            if (auto slice = cachedSlice(channelId, *tail, std::max(0, count - PageSize), count)) {
                emit messagesReceived({ .success = true,
                                        .type = LoadType::Latest,
                                        .channelId = channelId,
                                        .messages = *slice,
                                        .reachedLatest = true });
                return;
            }
        }
    }

    fetchPage(LoadType::Latest, channelId, Snowflake::Invalid, [this, channelId](Discord::Client::MessagesCallback done) {
        client->fetchLatestMessages(channelId, PageSize, std::move(done));
    });
}

void MessageManager::requestLoadHistory(Snowflake channelId, Snowflake beforeId)
{
    if (pagesInFlight.contains({ channelId, LoadType::History }))
        return;

    auto start = channelStartId.constFind(channelId);
    if (start != channelStartId.constEnd() && start.value() >= beforeId) {
        emit messagesReceived({ .success = true,
                                .type = LoadType::History,
                                .channelId = channelId,
                                .anchorId = beforeId });
        return;
    }

    if (auto pos = segments[channelId].find(beforeId); pos.run && pos.index > 0) {
        if (auto slice = cachedSlice(channelId, *pos.run, std::max(0, pos.index - PageSize), pos.index)) {
            emit messagesReceived({ .success = true,
                                    .type = LoadType::History,
                                    .channelId = channelId,
                                    .messages = *slice,
                                    .anchorId = beforeId });
            return;
        }
    }

    fetchPage(LoadType::History, channelId, beforeId, [this, channelId, beforeId](Discord::Client::MessagesCallback done) {
        client->fetchHistory(channelId, beforeId, PageSize, std::move(done));
    });
}

void MessageManager::requestLoadFuture(Snowflake channelId, Snowflake afterId)
{
    if (pagesInFlight.contains({ channelId, LoadType::Future }))
        return;

    if (auto pos = segments[channelId].find(afterId); pos.run) {
        int count = static_cast<int>(pos.run->ids.size());
        bool knowsWhatFollows = pos.index + 1 < count || pos.run->isTail;
        if (knowsWhatFollows) {
            int end = std::min(count, pos.index + 1 + PageSize);
            if (auto slice = cachedSlice(channelId, *pos.run, pos.index + 1, end)) {
                emit messagesReceived({ .success = true,
                                        .type = LoadType::Future,
                                        .channelId = channelId,
                                        .messages = *slice,
                                        .reachedLatest = pos.run->isTail && end == count,
                                        .anchorId = afterId });
                return;
            }
        }
    }

    fetchPage(LoadType::Future, channelId, afterId, [this, channelId, afterId](Discord::Client::MessagesCallback done) {
        client->fetchMessagesAfter(channelId, afterId, PageSize, std::move(done));
    });
}

void MessageManager::requestLoadAround(Snowflake channelId, Snowflake messageId)
{
    if (pagesInFlight.contains({ channelId, LoadType::Jump }))
        return;

    if (auto pos = segments[channelId].find(messageId); pos.run) {
        int count = static_cast<int>(pos.run->ids.size());
        int from = std::max(0, pos.index - JumpWindow / 2);
        int to = std::min(count, from + JumpWindow);
        if (auto slice = cachedSlice(channelId, *pos.run, from, to)) {
            emit messagesReceived({ .success = true,
                                    .type = LoadType::Jump,
                                    .channelId = channelId,
                                    .messages = *slice,
                                    .reachedLatest = pos.run->isTail && to == count });
            return;
        }
    }

    fetchPage(LoadType::Jump, channelId, messageId, [this, channelId, messageId](Discord::Client::MessagesCallback done) {
        client->fetchMessagesAround(channelId, messageId, JumpWindow, std::move(done));
    });
}

void MessageManager::fetchPage(LoadType type, Snowflake channelId, Snowflake anchorId, const PageFetcher &fetch)
{
    pagesInFlight.insert({ channelId, type });

    QPointer<MessageManager> guard = this;
    fetch([this, guard, type, channelId, anchorId](const Result<QList<Discord::Message>> &result) {
        if (!guard)
            return;

        pagesInFlight.remove({ channelId, type });

        if (!result.success()) {
            qCWarning(LogCore) << "Failed to load" << type << "page in" << channelId << result.error;
            emit messagesReceived({ .success = false,
                                    .type = type,
                                    .channelId = channelId,
                                    .anchorId = anchorId });
            return;
        }
        onApiMessagesReceived(result.value.value(), type, channelId, anchorId);
    });
}

std::optional<QList<Discord::Message>> MessageManager::cachedSlice(Snowflake channelId,
                                                                   const MessageSegments::Run &run,
                                                                   int from, int to)
{
    QList<Discord::Message> result;
    if (from >= to)
        return result;

    result.reserve(to - from);
    for (int i = from; i < to; i++) {
        auto *msg = messageCache.object(run.ids[i]);
        if (!msg) {
            result.clear();
            break;
        }
        result.append(*msg);
    }
    if (!result.isEmpty())
        return result;

    QList<Discord::Message> fromDisk = repo.getMessagesInRange(channelId, run.ids[from], run.ids[to - 1]);
    if (fromDisk.isEmpty())
        return std::nullopt;

    for (auto &msg : fromDisk)
        parseMessageContent(msg);
    return fromDisk;
}

static QList<Snowflake> idsOf(const QList<Discord::Message> &messages)
{
    QList<Snowflake> ids;
    ids.reserve(messages.size());
    for (const auto &msg : messages)
        ids.append(msg.id.get());
    return ids;
}

void MessageManager::onMessageCreated(const Discord::Message &message)
{
    onApiMessagesReceived({ message }, Discord::Client::MessageLoadType::Created,
                          message.channelId);
}

void MessageManager::onMessageUpdated(const Discord::Message &message)
{
    Discord::Message merged;
    bool haveBaseline = false;

    if (auto *cached = messageCache.object(message.id)) {
        merged = *cached;
        haveBaseline = true;
    } else if (auto existing = repo.getMessage(message.id)) {
        merged = *existing;
        haveBaseline = true;
    }

    if (haveBaseline) {
        merged.applyUpdate(message);
    } else {
        if (!message.presentKeys.contains(QStringLiteral("content"))) {
            return;
        }
        merged = message;
    }

    parseMessageContent(merged);

    messageCache.insert(merged.id, new Discord::Message(merged));
    repo.updateMessageContent(merged);

    if (message.presentKeys.contains(QStringLiteral("reactions")))
        repo.updateReactionsJson(merged.id, merged.reactionsJson);

    emit messagesReceived({ true, Discord::Client::MessageLoadType::Created, merged.channelId, { merged } });
}

void MessageManager::onMessageDeleted(const Discord::MessageDelete &event)
{
    Snowflake channelId = event.channelId.get();
    Snowflake messageId = event.id.get();

    messageCache.remove(messageId);

    if (segments.contains(channelId))
        segments[channelId].remove(messageId);

    repo.markMessageDeleted(messageId);

    emit messageDeleted(channelId, messageId);
}

void MessageManager::onMessageSendFailed(const QString &nonce, const QString &error)
{
    qCWarning(LogCore) << "Message send failed for nonce" << nonce << ":" << error;

    emit messageErrored(nonce);
}

void MessageManager::addReaction(Snowflake channelId, Snowflake messageId, const QString &emoji, bool isBurst)
{
    if (emojiManager)
        emojiManager->trackReaction(emoji);
    client->addReaction(channelId, messageId, emoji, isBurst);
}

void MessageManager::sendMessage(Snowflake channelId, const QString &content,
                                 Snowflake replyToMessageId,
                                 const QList<PendingAttachment> &attachments)
{
    if (emojiManager)
        emojiManager->trackMessageEmojis(content);

    Snowflake nonceId = Snowflake::generateNonce();
    QString nonce = QString::number(nonceId);

    auto outgoing = attachments;

    Discord::Message preview;
    preview.id = nonceId; // temporary id, will be overwritten
    preview.nonce = nonce;
    preview.channelId = channelId;
    preview.content = content;
    preview.timestamp = QDateTime::currentDateTimeUtc();
    preview.author = client->getMe();
    preview.flags = Discord::MessageFlags(0);
    preview.isPendingOutbound = true;

    if (!outgoing.isEmpty()) {
        QList<Discord::Attachment> previewAttachments;
        for (int i = 0; i < outgoing.size(); i++) {
            const auto &att = outgoing[i];
            Discord::Attachment a;
            a.id = Snowflake(nonceId + i + 1);
            a.filename = att.filename;
            a.size = att.size;
            a.contentType = att.mimeType;
            if (!att.image.isNull()) {
                a.localPreview = att.image;
                a.width = att.image.width();
                a.height = att.image.height();
            } else if (att.mimeType.startsWith("image/") && !att.filePath.isEmpty()) {
                QString localUrl = QUrl::fromLocalFile(att.filePath).toString();
                a.url = localUrl;
                a.proxyUrl = localUrl;
                QSize dims = QImageReader(att.filePath).size();
                if (dims.isValid()) {
                    a.width = dims.width();
                    a.height = dims.height();
                }
            }
            if (att.isSpoiler)
                a.flags = Discord::AttachmentFlags(Discord::AttachmentFlag::IS_SPOILER);
            previewAttachments.append(a);
        }
        preview.attachments = previewAttachments;
    }

    if (replyToMessageId.isValid()) {
        preview.type = Discord::MessageType::REPLY;
        Discord::MessageReference ref;
        ref.messageId = replyToMessageId;
        ref.channelId = channelId;
        preview.messageReference = ref;
    } else {
        preview.type = Discord::MessageType::DEFAULT;
    }

    auto ast = parser->parse(content, inlineParseState(channelId));
    bool jumbo = Markdown::Parser::isEmojiOnly(ast);
    preview.parsedContentCached = parser->toHtml(ast, jumbo);

    // get our fake preview in
    emit messagesReceived(
            { true, Discord::Client::MessageLoadType::Created, channelId, { preview } });

    for (const auto &att : outgoing) {
        if (att.data.isEmpty() && att.filePath.isEmpty()) {
            emit messageErrored(nonce);
            return;
        }
    }

    client->sendMessage(channelId, content, nonce, replyToMessageId, outgoing);
}

void MessageManager::cancelSend(Snowflake channelId, const QString &nonce)
{
    if (!client->cancelMessageSend(nonce))
        return;

    Snowflake nonceId(nonce.toULongLong());
    messageCache.remove(nonceId);
    if (segments.contains(channelId))
        segments[channelId].remove(nonceId);
    emit messageDeleted(channelId, nonceId);
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

void MessageManager::onApiMessagesReceived(const QList<Discord::Message> &messages, LoadType type,
                                           Snowflake channelId, Snowflake anchorId)
{
    repo.saveMessages(messages);

    auto sortedMessages = messages;
    std::sort(sortedMessages.begin(), sortedMessages.end(),
              [](const auto &a, const auto &b) { return a.id.get() < b.id.get(); });

    bool shortPage = sortedMessages.size() < PageSize;
    if (type == LoadType::History && shortPage)
        channelStartId[channelId] = sortedMessages.isEmpty() ? anchorId : sortedMessages.first().id.get();
    else if (type == LoadType::Latest && shortPage && !sortedMessages.isEmpty())
        channelStartId[channelId] = sortedMessages.first().id.get();

    for (auto &msg : sortedMessages)
        parseMessageContent(msg);

    for (const auto &msg : sortedMessages) {
        // cache owns its own copy
        Discord::Message *toCache = new Discord::Message(msg);
        messageCache.insert(toCache->id, toCache); // should remove old copies
    }

    auto &segs = segments[channelId];
    QList<Snowflake> ids = idsOf(sortedMessages);
    bool reachedLatest = false;

    switch (type) {
    case LoadType::Latest:
        fetchedChannels.insert(channelId);
        segs.setTail(ids);
        reachedLatest = true;
        break;
    case LoadType::Created:
        segs.merge(ids, Snowflake::Invalid, true);
        break;
    case LoadType::History:
        segs.merge(ids, anchorId, false);
        break;
    case LoadType::Future:
    case LoadType::Jump: {
        bool coversPresent = false;
        if (type == LoadType::Future) {
            coversPresent = sortedMessages.size() < PageSize;
        } else {
            int atOrAfterTarget = std::count_if(ids.cbegin(), ids.cend(),
                                                [&](Snowflake id) { return id >= anchorId; });
            coversPresent = atOrAfterTarget < JumpWindow / 2;
        }
        segs.merge(ids, anchorId, coversPresent);

        Snowflake newestLoaded = ids.isEmpty() ? anchorId : ids.last();
        auto pos = segs.find(newestLoaded);

        reachedLatest = pos.run ? pos.run->isTail : coversPresent;
        qCDebug(LogCore) << "Loaded" << ids.size() << "messages around/after" << anchorId << "in"
                         << channelId << "coversPresent" << coversPresent << "reachedLatest" << reachedLatest;

        if (pos.run && reachedLatest) {
            for (int i = pos.index + 1; i < static_cast<int>(pos.run->ids.size()); ++i) {
                auto *cached = messageCache.object(pos.run->ids[i]);
                if (!cached)
                    break;
                sortedMessages.append(*cached);
            }
        }
        break;
    }
    }

    emit messagesReceived({ .success = true,
                            .type = type,
                            .channelId = channelId,
                            .messages = sortedMessages,
                            .reachedLatest = reachedLatest,
                            .anchorId = anchorId });
}

} // namespace Core
} // namespace Acheron
