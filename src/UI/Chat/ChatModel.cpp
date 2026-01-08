#include "ChatModel.hpp"

#include "Core/Markdown/Parser.hpp"
#include "Core/MessageManager.hpp"
#include "Core/ImageManager.hpp"
#include "Core/AttachmentCache.hpp"

namespace Acheron {
namespace UI {

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

ChatModel::ChatModel(Core::ImageManager *imageManager, Core::AttachmentCache *attachmentCache,
                     QObject *parent)
    : QAbstractListModel(parent), imageManager(imageManager), attachmentCache(attachmentCache)
{
    connect(imageManager, &Core::ImageManager::imageFetched, this,
            [this](const QUrl &url, const QSize &size, const QPixmap &pixmap) {
                auto values = pendingRequests.values(url);
                for (const auto &index : values) {
                    if (index.isValid())
                        emit dataChanged(index, index, { Qt::DecorationRole });
                }
            });

    connect(attachmentCache, &Core::AttachmentCache::attachmentFetched, this,
            [this](const QUrl &url, const QPixmap &pixmap) {
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
    case UsernameRole:
        return msg.author->getDisplayName();
    case AvatarRole: {
        const QSize desiredSize(32, 32);

        if (!avatarUrlResolver)
            return imageManager->placeholder(desiredSize);

        QUrl url = avatarUrlResolver(msg.author.get());
        QPixmap pixmap = imageManager->get(url, desiredSize);

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
    case UserIdRole:
        return msg.author->id;
    case CachedSizeRole: {
        if (sizeCache.contains(msg.id))
            return sizeCache.value(msg.id);
        return {};
    }
    case ShowHeaderRole: {
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
        return msg.parsedContentCached;
    }
    case AttachmentsRole: {
        if (!msg.attachments.hasValue() || msg.attachments->isEmpty())
            return QVariant();

        QList<AttachmentData> result;
        for (const auto &att : *msg.attachments) {
            AttachmentData data;
            data.proxyUrl = QUrl(*att.proxyUrl);
            data.originalUrl = QUrl(*att.url);
            data.isImage = att.isImage();
            data.filename = att.filename.hasValue() ? *att.filename : "unknown";
            data.fileSizeBytes = att.size.hasValue() ? *att.size : 0;

            if (att.isImage()) {
                QSize original;
                if (att.width.hasValue() && att.height.hasValue())
                    original = QSize(*att.width, *att.height);

                data.displaySize = Core::AttachmentCache::calculateDisplaySize(original);
                data.pixmap = attachmentCache->get(data.proxyUrl, original);
                data.isLoading = !attachmentCache->isCached(data.proxyUrl);
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
                imageData.displaySize = Core::AttachmentCache::calculateDisplaySize(origSize);
                imageData.pixmap = attachmentCache->get(imageData.url, origSize);

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

                if (embed.author.hasValue()) {
                    data.authorName =
                            embed.author->name.hasValue() ? *embed.author->name : QString();
                    data.authorUrl = embed.author->proxyIconUrl.hasValue()
                                             ? *embed.author->proxyIconUrl
                                             : QString();
                    if (embed.author->iconUrl.hasValue()) {
                        data.authorIconUrl = QUrl(*embed.author->proxyIconUrl);
                        data.authorIcon = attachmentCache->get(data.authorIconUrl, QSize(24, 24));
                    }
                }

                if (embed.footer.hasValue()) {
                    data.footerText =
                            embed.footer->text.hasValue() ? *embed.footer->text : QString();
                    if (embed.footer->proxyIconUrl.hasValue()) {
                        data.footerIconUrl = QUrl(*embed.footer->proxyIconUrl);
                        data.footerIcon = attachmentCache->get(data.footerIconUrl, QSize(20, 20));
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
                    data.thumbnailSize = origSize.isValid()
                                                 ? origSize.scaled(80, 80, Qt::KeepAspectRatio)
                                                 : QSize(80, 80);
                    data.thumbnail = attachmentCache->get(data.thumbnailUrl, origSize);
                }

                if (hasImage) {
                    EmbedImageData imageData;
                    imageData.url = QUrl(*embed.image->proxyUrl);
                    QSize origSize;
                    if (embed.image->width.hasValue() && embed.image->height.hasValue())
                        origSize = QSize(*embed.image->width, *embed.image->height);
                    imageData.displaySize = Core::AttachmentCache::calculateDisplaySize(origSize);
                    imageData.pixmap = attachmentCache->get(imageData.url, origSize);
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
                                Core::AttachmentCache::calculateDisplaySize(origSize);
                        data.videoThumbnail =
                                attachmentCache->get(data.videoThumbnailUrl, origSize);
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
                        data.fields.append(fieldData);
                    }
                }

                if (!embedUrl.isEmpty())
                    urlToEmbedIndex[embedUrl] = result.size();

                if (hasAnything)
                    result.append(data);
            }
        }

        return QVariant::fromValue(result);
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

    switch (result.type) {
    case Discord::Client::MessageLoadType::Latest: {
        beginResetModel();
        sizeCache.clear();
        messages = result.messages;
        endResetModel();
        break;
    };
    case Discord::Client::MessageLoadType::History: {
        beginInsertRows({}, 0, result.messages.size() - 1);
        messages = result.messages + messages;
        endInsertRows();
        break;
    }
    case Discord::Client::MessageLoadType::Created: {
        beginInsertRows({}, messages.size(), messages.size() + result.messages.size() - 1);
        messages = messages + result.messages;
        endInsertRows();
        break;
    }
    default:
        break;
    }
}

void ChatModel::setActiveChannel(Snowflake channelId)
{
    if (currentChannelId == channelId)
        return;

    currentChannelId = channelId;

    beginResetModel();
    messages.clear();
    sizeCache.clear();
    endResetModel();
}

} // namespace UI
} // namespace Acheron
