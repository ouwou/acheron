#include "ChatModel.hpp"

#include "Core/Markdown/Parser.hpp"
#include "Core/MessageManager.hpp"
#include "Core/ImageManager.hpp"
#include "Discord/Enums.hpp"

namespace Acheron {
namespace UI {

static bool isSystemMessageType(Discord::MessageType type)
{
    switch (type) {
    case Discord::MessageType::DEFAULT:
    case Discord::MessageType::REPLY:
    case Discord::MessageType::CHAT_INPUT_COMMAND:
    case Discord::MessageType::CONTEXT_MENU_COMMAND:
        return false;
    default:
        return true;
    }
}

static EmbedType embedTypeFromString(const QString &typeStr)
{
    if (typeStr.isEmpty() || typeStr == "rich")
        return EmbedType::Rich;
    else if (typeStr == "age_verification_system_notification")
        return EmbedType::AgeVerificationSystemNotification;
    else if (typeStr == "application_news")
        return EmbedType::ApplicationNews;
    else if (typeStr == "article")
        return EmbedType::Article;
    else if (typeStr == "auto_moderation_message")
        return EmbedType::AutoModerationMessage;
    else if (typeStr == "auto_moderation_notification")
        return EmbedType::AutoModerationNotification;
    else if (typeStr == "gift")
        return EmbedType::Gift;
    else if (typeStr == "gifv")
        return EmbedType::Gifv;
    else if (typeStr == "image")
        return EmbedType::Image;
    else if (typeStr == "link")
        return EmbedType::Link;
    else if (typeStr == "poll_result")
        return EmbedType::PollResult;
    else if (typeStr == "post_preview")
        return EmbedType::PostPreview;
    else if (typeStr == "rich")
        return EmbedType::Rich;
    else if (typeStr == "safety_policy_notice")
        return EmbedType::SafetyPolicyNotice;
    else if (typeStr == "safety_system_notification")
        return EmbedType::SafetySystemNotification;
    else if (typeStr == "video")
        return EmbedType::Video;
    return EmbedType::Rich;
}

ChatModel::ChatModel(Core::ImageManager *imageManager, QObject *parent)
    : QAbstractListModel(parent), imageManager(imageManager)
{
    connect(imageManager, &Core::ImageManager::imageFetched, this,
            [this](const QUrl &url, const QSize &size, const QPixmap &pixmap) {
                // avatar pending requests
                auto values = pendingRequests.values(url);
                for (const auto &index : values) {
                    if (index.isValid())
                        emit dataChanged(index, index, { Qt::DecorationRole });
                }

                // custom emoji in message content and embed text
                if (url.host() == u"cdn.discordapp.com" && url.path().startsWith(u"/emojis/")) {
                    QString urlStr = url.toString();
                    for (int row = 0; row < messages.size(); ++row) {
                        const auto &msg = messages[row];
                        bool found = msg.parsedContentCached.contains(urlStr);
                        if (!found && embedCache.contains(msg.id)) {
                            for (const auto &embed : embedCache.value(msg.id)) {
                                if (embed.titleParsed.contains(urlStr) ||
                                    embed.descriptionParsed.contains(urlStr)) {
                                    found = true;
                                    break;
                                }
                                for (const auto &field : embed.fields) {
                                    if (field.nameParsed.contains(urlStr) ||
                                        field.valueParsed.contains(urlStr)) {
                                        found = true;
                                        break;
                                    }
                                }
                                if (found)
                                    break;
                            }
                        }

                        bool reactionFound = false;
                        if (!found && msg.reactions.hasValue()) {
                            for (const auto &reaction : *msg.reactions) {
                                if (!reaction.emoji->isUnicode()) {
                                    QString emojiUrl = reaction.emoji->getImageUrl(48);
                                    if (emojiUrl == urlStr) {
                                        reactionFound = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (found) {
                            invalidateDocCacheForMessage(msg.id);
                            QModelIndex idx = index(row, 0);
                            emit dataChanged(idx, idx, { HtmlRole, EmbedsRole, CachedSizeRole });
                        } else if (reactionFound) {
                            sizeCache.remove(msg.id);
                            QModelIndex idx = index(row, 0);
                            emit dataChanged(idx, idx, { ReactionsRole, CachedSizeRole });
                        }
                    }
                    return;
                }

                // attachment and embed images
                for (int row = 0; row < messages.size(); ++row) {
                    const auto &msg = messages[row];
                    bool found = false;

                    if (msg.attachments.hasValue()) {
                        for (const auto &att : *msg.attachments) {
                            if (QUrl(*att.proxyUrl) == url) {
                                found = true;
                                break;
                            }
                        }
                    }

                    if (!found && msg.embeds.hasValue()) {
                        for (const auto &embed : *msg.embeds) {
                            if (embed.author.hasValue() && embed.author->proxyIconUrl.hasValue() &&
                                QUrl(*embed.author->proxyIconUrl) == url) {
                                found = true;
                                break;
                            }
                            if (embed.footer.hasValue() && embed.footer->proxyIconUrl.hasValue() &&
                                QUrl(*embed.footer->proxyIconUrl) == url) {
                                found = true;
                                break;
                            }
                            if (embed.thumbnail.hasValue() &&
                                embed.thumbnail->proxyUrl.hasValue() &&
                                QUrl(*embed.thumbnail->proxyUrl) == url) {
                                found = true;
                                break;
                            }

                            if (embed.image.hasValue() && embed.image->proxyUrl.hasValue() &&
                                QUrl(*embed.image->proxyUrl) == url) {
                                found = true;
                                break;
                            }
                        }
                    }

                    if (found) {
                        embedCache.remove(msg.id);
                        invalidateDocCacheForMessage(msg.id);
                        QModelIndex idx = index(row, 0);
                        emit dataChanged(idx, idx, { AttachmentsRole, EmbedsRole, CachedSizeRole });
                    }
                }
            });
}

void ChatModel::setAvatarUrlResolver(AvatarUrlResolver resolver)
{
    avatarUrlResolver = std::move(resolver);
}

void ChatModel::setDisplayNameResolver(DisplayNameResolver resolver)
{
    displayNameResolver = std::move(resolver);
}

void ChatModel::setRoleColorResolver(RoleColorResolver resolver)
{
    roleColorResolver = std::move(resolver);
}

int ChatModel::rowCount(const QModelIndex &parent) const
{
    return messages.size();
}

QVariant ChatModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    const auto &msg = messages[index.row()];
    switch (role) {
    case Qt::DisplayRole:
        [[fallthrough]];
    case ContentRole:
        return msg.content;
    case UsernameRole: {
        if (displayNameResolver) {
            QString name = displayNameResolver(msg.author->id.get(), currentGuildId);
            if (!name.isEmpty())
                return name;
        }
        return msg.author->getDisplayName();
    }
    case AvatarRole: {
        const QSize desiredSize(32, 32);

        if (!avatarUrlResolver)
            return imageManager->placeholder(desiredSize);

        QUrl url = avatarUrlResolver(msg.author.get());
        QPixmap pixmap = imageManager->get(url, desiredSize, Core::PinGroup::ChatView);

        if (!imageManager->isCached(url, desiredSize)) {
            bool alreadyWaiting = false;
            auto it = pendingRequests.constFind(url);
            while (it != pendingRequests.cend() && it.key() == url) {
                if (it.value() == index) {
                    alreadyWaiting = true;
                    break;
                }
                it++;
            }

            if (!alreadyWaiting)
                pendingRequests.insert(url, QPersistentModelIndex(index));
        }

        return pixmap;
    }
    case TimestampRole:
        return msg.timestamp;
    case EditedTimestampRole:
        return msg.editedTimestamp.hasValue() ? QVariant(*msg.editedTimestamp) : QVariant();
    case UserIdRole:
        return msg.author->id;
    case CachedSizeRole: {
        if (sizeCache.contains(msg.id))
            return sizeCache.value(msg.id);
        return {};
    }
    case ShowHeaderRole: {
        if (isSystemMessageType(msg.type))
            return false;

        // replies always show a header
        if (msg.type == Discord::MessageType::REPLY)
            return true;

        if (index.row() == 0)
            return true;

        const auto &prevMsg = messages[index.row() - 1];

        if (prevMsg.author->id != msg.author->id)
            return true;

        if (prevMsg.timestamp->date() != msg.timestamp->date())
            return true;

        return false;
    }
    case DateSeparatorRole: {
        if (index.row() == 0)
            return true;

        const auto &prevMsg = messages[index.row() - 1];

        if (prevMsg.timestamp->date() != msg.timestamp->date())
            return true;

        return false;
    }
    case HtmlRole: {
        // for image embeds, suppress text if content is just the embed url
        if (msg.embeds.hasValue() && msg.embeds->size() == 1) {
            const auto &embed = msg.embeds->first();
            QString embedType = embed.type.hasValue() ? *embed.type : QString();
            if (embedType == "image") {
                QString embedUrl = embed.url.hasValue() ? *embed.url : QString();
                if (!embedUrl.isEmpty() && msg.content == embedUrl)
                    return QString();
            }
        }
        return msg.parsedContentCached;
    }
    case AttachmentsRole: {
        if (!msg.attachments.hasValue() || msg.attachments->isEmpty())
            return QVariant();

        QList<AttachmentData> result;
        for (const auto &att : *msg.attachments) {
            AttachmentData data;
            data.id = att.id;
            data.proxyUrl = QUrl(*att.proxyUrl);
            data.originalUrl = QUrl(*att.url);
            data.isImage = att.isImage();
            data.filename = att.filename.hasValue() ? *att.filename : "unknown";
            data.fileSizeBytes = att.size.hasValue() ? *att.size : 0;
            data.isSpoiler = att.isSpoiler();

            if (att.isImage()) {
                QSize original;
                if (att.width.hasValue() && att.height.hasValue())
                    original = QSize(*att.width, *att.height);

                data.displaySize = Core::ImageManager::calculateDisplaySize(original);
                data.pixmap = suppressImageFetch
                                      ? imageManager->getIfCached(data.proxyUrl, data.displaySize)
                                      : imageManager->get(data.proxyUrl, data.displaySize);
                data.isLoading = !imageManager->isCached(data.proxyUrl, data.displaySize);
            } else {
                data.displaySize = QSize();
                data.isLoading = false;
            }

            result.append(data);
        }

        return QVariant::fromValue(result);
    }
    case EmbedsRole: {
        if (!msg.embeds.hasValue() || msg.embeds->isEmpty())
            return QVariant();

        if (embedCache.contains(msg.id))
            return QVariant::fromValue(embedCache.value(msg.id));

        QList<EmbedData> result;
        // for handling the url-based embed image merging
        QMap<QString, int> urlToEmbedIndex;

        for (const auto &embed : *msg.embeds) {
            QString embedUrl = embed.url.hasValue() ? *embed.url : QString();

            bool hasImage = embed.image.hasValue() && embed.image->proxyUrl.hasValue() &&
                            embed.image->contentType.hasValue() &&
                            embed.image->contentType->startsWith("image/");

            bool shouldMerge = false;
            int parentIndex = -1;

            if (!embedUrl.isEmpty() && hasImage && urlToEmbedIndex.contains(embedUrl)) {
                parentIndex = urlToEmbedIndex[embedUrl];
                // excess ignored
                if (result[parentIndex].images.size() < 4)
                    shouldMerge = true;
            }

            if (shouldMerge) {
                EmbedImageData imageData;
                imageData.url = QUrl(*embed.image->proxyUrl);
                QSize origSize;
                if (embed.image->width.hasValue() && embed.image->height.hasValue())
                    origSize = QSize(*embed.image->width, *embed.image->height);
                imageData.displaySize = Core::ImageManager::calculateDisplaySize(origSize);
                imageData.pixmap =
                        suppressImageFetch
                                ? imageManager->getIfCached(imageData.url, imageData.displaySize)
                                : imageManager->get(imageData.url, imageData.displaySize);

                result[parentIndex].images.append(imageData);
            } else if (!shouldMerge && hasImage && !embedUrl.isEmpty() &&
                       urlToEmbedIndex.contains(embedUrl)) {
                continue;
            } else {
                EmbedData data;

                bool hasAnything = embed.title.hasValue() || embed.description.hasValue() ||
                                   embed.timestamp.hasValue() || embed.color.hasValue() ||
                                   embed.author.hasValue() || embed.footer.hasValue() || hasImage;

                data.type = embedTypeFromString(embed.type.hasValue() ? *embed.type : QString());
                data.title = embed.title.hasValue() ? *embed.title : QString();
                data.description = embed.description.hasValue() ? *embed.description : QString();
                data.url = embedUrl;
                data.timestamp = embed.timestamp.hasValue() ? *embed.timestamp : QDateTime();
                data.color = embed.color.hasValue() ? QColor::fromRgb(*embed.color)
                                                    : QColor(88, 101, 242);

                static Core::Markdown::Parser parser;
                Core::Markdown::ParseState titleState;
                titleState.isInline = true;
                titleState.excludedRules.insert("link");
                if (!data.title.isEmpty()) {
                    auto ast = parser.parse(data.title, titleState);
                    data.titleParsed = parser.toHtml(ast);
                }

                Core::Markdown::ParseState descriptionState;
                descriptionState.isInline = true;
                if (!data.description.isEmpty()) {
                    auto ast = parser.parse(data.description, descriptionState);
                    data.descriptionParsed = parser.toHtml(ast);
                }

                if (embed.author.hasValue()) {
                    data.authorName =
                            embed.author->name.hasValue() ? *embed.author->name : QString();
                    data.authorUrl = embed.author->proxyIconUrl.hasValue()
                                             ? *embed.author->proxyIconUrl
                                             : QString();
                    if (embed.author->iconUrl.hasValue()) {
                        data.authorIconUrl = QUrl(*embed.author->proxyIconUrl);
                        data.authorIcon =
                                suppressImageFetch
                                        ? imageManager->getIfCached(data.authorIconUrl,
                                                                    QSize(24, 24))
                                        : imageManager->get(data.authorIconUrl, QSize(24, 24));
                    }
                }

                if (embed.footer.hasValue()) {
                    data.footerText =
                            embed.footer->text.hasValue() ? *embed.footer->text : QString();
                    if (embed.footer->proxyIconUrl.hasValue()) {
                        data.footerIconUrl = QUrl(*embed.footer->proxyIconUrl);
                        data.footerIcon =
                                suppressImageFetch
                                        ? imageManager->getIfCached(data.footerIconUrl,
                                                                    QSize(20, 20))
                                        : imageManager->get(data.footerIconUrl, QSize(20, 20));
                    }
                }

                if (embed.provider.hasValue()) {
                    data.providerName =
                            embed.provider->name.hasValue() ? *embed.provider->name : QString();
                    data.providerUrl =
                            embed.provider->url.hasValue() ? *embed.provider->url : QString();
                }

                // observed png thumbnail with width/height but no content type
                if (embed.thumbnail.hasValue() && embed.thumbnail->proxyUrl.hasValue() &&
                    embed.thumbnail->width > 0) {
                    hasAnything = true;
                    data.thumbnailUrl = QUrl(*embed.thumbnail->proxyUrl);
                    QSize origSize;
                    if (embed.thumbnail->width.hasValue() && embed.thumbnail->height.hasValue())
                        origSize = QSize(*embed.thumbnail->width, *embed.thumbnail->height);

                    if (data.type == EmbedType::Gifv || data.type == EmbedType::Image)
                        data.thumbnailSize = Core::ImageManager::calculateDisplaySize(origSize);
                    else
                        data.thumbnailSize = origSize.isValid()
                                                     ? origSize.scaled(80, 80, Qt::KeepAspectRatio)
                                                     : QSize(80, 80);
                    data.thumbnail =
                            suppressImageFetch
                                    ? imageManager->getIfCached(data.thumbnailUrl,
                                                                data.thumbnailSize)
                                    : imageManager->get(data.thumbnailUrl, data.thumbnailSize);
                }

                if (hasImage) {
                    EmbedImageData imageData;
                    imageData.url = QUrl(*embed.image->proxyUrl);
                    QSize origSize;
                    if (embed.image->width.hasValue() && embed.image->height.hasValue())
                        origSize = QSize(*embed.image->width, *embed.image->height);
                    imageData.displaySize = Core::ImageManager::calculateDisplaySize(origSize);
                    imageData.pixmap =
                            suppressImageFetch
                                    ? imageManager->getIfCached(imageData.url,
                                                                imageData.displaySize)
                                    : imageManager->get(imageData.url, imageData.displaySize);
                    data.images.append(imageData);
                }

                if (embed.video.hasValue()) {
                    if (embed.thumbnail.hasValue() && embed.thumbnail->proxyUrl.hasValue() &&
                        embed.thumbnail->proxyUrl->startsWith("https://")) {
                        hasAnything = true;
                        data.videoThumbnailUrl = QUrl(*embed.thumbnail->proxyUrl);
                        QSize origSize;
                        if (embed.thumbnail->width.hasValue() && embed.thumbnail->height.hasValue())
                            origSize = QSize(*embed.thumbnail->width, *embed.thumbnail->height);
                        data.videoThumbnailSize =
                                Core::ImageManager::calculateDisplaySize(origSize);
                        data.videoThumbnail =
                                suppressImageFetch
                                        ? imageManager->getIfCached(data.videoThumbnailUrl,
                                                                    data.videoThumbnailSize)
                                        : imageManager->get(data.videoThumbnailUrl,
                                                            data.videoThumbnailSize);
                    }
                }

                if (embed.fields.hasValue()) {
                    if (!embed.fields->empty())
                        hasAnything = true;
                    for (const auto &field : *embed.fields) {
                        EmbedFieldData fieldData;
                        fieldData.name = field.name.hasValue() ? *field.name : QString();
                        fieldData.value = field.value.hasValue() ? *field.value : QString();
                        fieldData.isInline = field.isInline.hasValue() ? *field.isInline : false;

                        Core::Markdown::ParseState nameState;
                        nameState.isInline = true;
                        nameState.excludedRules.insert("link");
                        if (!fieldData.name.isEmpty()) {
                            auto ast = parser.parse(fieldData.name, nameState);
                            fieldData.nameParsed = parser.toHtml(ast);
                        }

                        Core::Markdown::ParseState valueState;
                        valueState.isInline = true;
                        if (!fieldData.value.isEmpty()) {
                            auto ast = parser.parse(fieldData.value, valueState);
                            fieldData.valueParsed = parser.toHtml(ast);
                        }

                        data.fields.append(fieldData);
                    }
                }

                if (!embedUrl.isEmpty())
                    urlToEmbedIndex[embedUrl] = result.size();

                if (hasAnything)
                    result.append(data);
            }
        }

        if (!suppressImageFetch)
            embedCache[msg.id] = result;
        return QVariant::fromValue(result);
    }
    case IsPendingRole:
        return msg.nonce.hasValue() && pendingNonces.contains(msg.nonce.get());
    case IsErroredRole:
        return msg.nonce.hasValue() && erroredNonces.contains(msg.nonce.get());
    case UsernameColorRole: {
        if (!roleColorResolver || currentGuildId == Snowflake::Invalid)
            return QColor();
        return roleColorResolver(msg.author->id.get(), currentGuildId);
    }
    case MessageIdRole:
        return msg.id;
    case ReactionsRole: {
        if (!msg.reactions.hasValue() || msg.reactions->isEmpty())
            return QVariant();

        QList<ReactionData> result;
        for (const auto &reaction : *msg.reactions) {
            QPixmap emojiPixmap;
            bool isLoading = false;
            Core::Snowflake emojiId;
            if (!reaction.emoji->isUnicode()) {
                emojiId = reaction.emoji->id;
                QString emojiUrl = reaction.emoji->getImageUrl(48);
                QSize emojiSize(16, 16);
                emojiPixmap = imageManager->get(QUrl(emojiUrl), emojiSize);
                isLoading = !imageManager->isCached(QUrl(emojiUrl), emojiSize);
            }

            int normalCount = reaction.countDetails.hasValue() ? *reaction.countDetails->normal : *reaction.count;
            int burstCount = reaction.countDetails.hasValue() ? *reaction.countDetails->burst : 0;

            if (burstCount > 0) {
                ReactionData data;
                data.emojiName = reaction.emoji->name;
                data.emojiId = emojiId;
                data.emojiAnimated = reaction.emoji->animated.hasValue() && *reaction.emoji->animated;
                data.count = burstCount;
                data.me = reaction.meBurst.hasValue() && *reaction.meBurst;
                data.isBurst = true;
                data.emojiPixmap = emojiPixmap;
                data.isLoading = isLoading;
                data.burstTintColor = reaction.getBrightestBurstColor();
                result.append(data);
            }

            if (normalCount > 0) {
                ReactionData data;
                data.emojiName = reaction.emoji->name;
                data.emojiId = emojiId;
                data.emojiAnimated = reaction.emoji->animated.hasValue() && *reaction.emoji->animated;
                data.count = normalCount;
                data.me = reaction.me;
                data.isBurst = false;
                data.emojiPixmap = emojiPixmap;
                data.isLoading = isLoading;
                result.append(data);
            }
        }

        return QVariant::fromValue(result);
    }
    case IsSystemMessageRole:
        return isSystemMessageType(msg.type);
    case ReplyDataRole: {
        ReplyData reply;

        if (msg.type != Discord::MessageType::REPLY) {
            reply.state = ReplyData::State::None;
            return QVariant::fromValue(reply);
        }

        if (!msg.referencedMessage) {
            if (msg.referencedMessageNull) {
                reply.state = ReplyData::State::Deleted;
            } else {
                reply.state = ReplyData::State::Unknown;
            }
            if (msg.messageReference.hasValue() && msg.messageReference->messageId.hasValue())
                reply.referencedMessageId = *msg.messageReference->messageId;
            return QVariant::fromValue(reply);
        }

        const auto &ref = msg.referencedMessage;
        reply.state = ReplyData::State::Present;
        reply.referencedMessageId = ref->id;
        reply.authorId = ref->author->id;
        reply.contentSnippet = ref->content;

        if (roleColorResolver && currentGuildId != Snowflake::Invalid)
            reply.authorColor = roleColorResolver(ref->author->id.get(), currentGuildId);

        // resolve display name via the same resolver used for messages
        if (displayNameResolver) {
            QString name = displayNameResolver(ref->author->id.get(), currentGuildId);
            if (!name.isEmpty()) {
                reply.authorName = name;
                return QVariant::fromValue(reply);
            }
        }
        reply.authorName = ref->author->getDisplayName();

        return QVariant::fromValue(reply);
    }
    default:
        return {};
    }
}

bool ChatModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid())
        return false;

    if (role == CachedSizeRole) {
        auto &msg = messages[index.row()];
        sizeCache[msg.id] = value.toSize();
        return true;
    }

    return false;
}

Snowflake ChatModel::getOldestMessageId() const
{
    if (messages.isEmpty())
        return Snowflake::Invalid;
    return messages.first().id;
}

Snowflake ChatModel::getActiveChannelId() const
{
    return currentChannelId;
}

void ChatModel::setMessages(const QList<Discord::Message> &messages) { }

void ChatModel::handleIncomingMessages(const Core::MessageRequestResult &result)
{
    if (!result.success)
        return;

    if (result.channelId != currentChannelId)
        return;

    if (result.messages.isEmpty())
        return;

    auto incomingMessages = result.messages;
    std::sort(incomingMessages.begin(), incomingMessages.end(),
              [](const Discord::Message &a, const Discord::Message &b) {
                  return a.id.get() < b.id.get();
              });

    switch (result.type) {
    case Discord::Client::MessageLoadType::Latest: {
        beginResetModel();
        sizeCache.clear();
        embedCache.clear();
        docCache.clear();
        pendingNonces.clear();
        erroredNonces.clear();
        messages = incomingMessages;
        endResetModel();
        break;
    };
    case Discord::Client::MessageLoadType::History: {
        int numNew = incomingMessages.size();

        const Snowflake oldAnchorId = messages.first().id;

        beginInsertRows({}, 0, numNew - 1);
        messages = incomingMessages + messages;
        endInsertRows();

        // invalidate cached size cuz header and/or separator might have moved
        sizeCache.remove(oldAnchorId);
        QModelIndex oldAnchorIdx = index(numNew, 0);
        emit dataChanged(oldAnchorIdx, oldAnchorIdx,
                         { CachedSizeRole, ShowHeaderRole, DateSeparatorRole });

        break;
    }
    case Discord::Client::MessageLoadType::Created: {
        bool isUpdate = false;
        for (const auto &incomingMsg : incomingMessages) {
            for (int i = 0; i < messages.size(); i++) {
                // update
                if (messages[i].id == incomingMsg.id) {
                    messages[i] = incomingMsg;

                    sizeCache.remove(incomingMsg.id);
                    embedCache.remove(incomingMsg.id);
                    invalidateDocCacheForMessage(incomingMsg.id);

                    QModelIndex idx = index(i, 0);
                    emit dataChanged(idx, idx);
                    isUpdate = true;
                    break;
                }
            }
        }

        if (isUpdate)
            break;

        // replace sent message by nonce
        bool replacedPreview = false;
        for (const auto &incomingMsg : incomingMessages) {
            if (incomingMsg.nonce.hasValue()) {
                QString nonce = incomingMsg.nonce.get();

                // todo qhash? probably doesnt matter at all
                for (int i = 0; i < messages.size(); i++) {
                    if (messages[i].nonce.hasValue() && messages[i].nonce.get() == nonce) {
                        messages[i] = incomingMsg;
                        pendingNonces.remove(nonce);
                        QModelIndex idx = index(i, 0);
                        emit dataChanged(idx, idx);
                        replacedPreview = true;
                        break;
                    }
                }
            }
        }

        if (!replacedPreview) {
            beginInsertRows({}, messages.size(), messages.size() + incomingMessages.size() - 1);

            for (const auto &msg : incomingMessages) {
                if (msg.isPendingOutbound && msg.nonce.hasValue()) {
                    pendingNonces.insert(msg.nonce.get());
                }
            }

            messages = messages + incomingMessages;
            endInsertRows();
        }
        break;
    }
    default:
        break;
    }
}

void ChatModel::handleMessageDeleted(Snowflake channelId, Snowflake messageId)
{
    if (channelId != currentChannelId)
        return;

    for (int i = 0; i < messages.size(); i++) {
        if (messages[i].id == messageId) {
            beginRemoveRows({}, i, i);
            sizeCache.remove(messageId);
            embedCache.remove(messageId);
            invalidateDocCacheForMessage(messageId);
            messages.remove(i);
            endRemoveRows();

            // invalidate what came afterwards
            if (i < messages.size()) {
                const auto &nextMessage = messages[i];

                sizeCache.remove(nextMessage.id);
                embedCache.remove(nextMessage.id);
                invalidateDocCacheForMessage(nextMessage.id);

                QModelIndex idx = index(i, 0);
                emit dataChanged(idx, idx, { CachedSizeRole, ShowHeaderRole, DateSeparatorRole });
            }
            break;
        }
    }
}

void ChatModel::handleMessageErrored(const QString &nonce)
{
    for (int i = 0; i < messages.size(); i++) {
        if (messages[i].nonce.hasValue() && messages[i].nonce.get() == nonce) {
            pendingNonces.remove(nonce);
            erroredNonces.insert(nonce);
            QModelIndex idx = index(i, 0);
            emit dataChanged(idx, idx);
            break;
        }
    }
}

void ChatModel::setActiveChannel(Snowflake channelId, Snowflake guildId)
{
    if (currentChannelId == channelId)
        return;

    imageManager->unpinGroup(Core::PinGroup::ChatView);

    currentChannelId = channelId;
    currentGuildId = guildId;

    beginResetModel();
    messages.clear();
    sizeCache.clear();
    embedCache.clear();
    docCache.clear();
    pendingNonces.clear();
    erroredNonces.clear();
    revealedSpoilers.clear();
    endResetModel();
}

void ChatModel::refreshUsersInView(const QList<Snowflake> &userIds)
{
    bool refreshAll = userIds.isEmpty();

    for (int row = 0; row < messages.size(); ++row) {
        const auto &msg = messages[row];
        if (!msg.author.hasValue())
            continue;

        Snowflake authorId = msg.author->id.get();

        if (refreshAll || userIds.contains(authorId)) {
            QModelIndex idx = index(row, 0);
            emit dataChanged(idx, idx, { UsernameRole, UsernameColorRole });
        }
    }
}

void ChatModel::revealSpoiler(Snowflake attachmentId)
{
    if (revealedSpoilers.contains(attachmentId))
        return;

    revealedSpoilers.insert(attachmentId);

    for (int row = 0; row < messages.size(); ++row) {
        const auto &msg = messages[row];
        if (msg.attachments.hasValue()) {
            for (const auto &att : *msg.attachments) {
                if (*att.id == attachmentId) {
                    QModelIndex idx = index(row, 0);
                    emit dataChanged(idx, idx, { AttachmentsRole, CachedSizeRole });
                    return;
                }
            }
        }
    }
}

bool ChatModel::isSpoilerRevealed(Snowflake attachmentId) const
{
    return revealedSpoilers.contains(attachmentId);
}

QTextDocument *ChatModel::getCachedDocument(const DocCacheKey &key) const
{
    return docCache.object(key);
}

void ChatModel::cacheDocument(const DocCacheKey &key, QTextDocument *doc) const
{
    docCache.insert(key, doc);
}

void ChatModel::invalidateDocCache()
{
    docCache.clear();
    docCacheWidth = 0;
}

void ChatModel::invalidateDocCacheForMessage(Snowflake messageId)
{
    docCache.remove(bodyDocKey(messageId));

    // brute remove all possible entries
    for (int ei = 0; ei < 10; ++ei) {
        docCache.remove(embedTitleDocKey(messageId, ei));
        docCache.remove(embedDescDocKey(messageId, ei));
        for (int fi = 0; fi < 25; ++fi) {
            docCache.remove(embedFieldNameDocKey(messageId, ei, fi));
            docCache.remove(embedFieldValueDocKey(messageId, ei, fi));
        }
    }
}

} // namespace UI
} // namespace Acheron
