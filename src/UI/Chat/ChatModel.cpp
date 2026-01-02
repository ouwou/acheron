#include "ChatModel.hpp"

#include "Core/Markdown/Parser.hpp"
#include "Core/MessageManager.hpp"
#include "Core/ImageManager.hpp"

namespace Acheron {
namespace UI {
ChatModel::ChatModel(Core::ImageManager *imageManager, QObject *parent)
    : QAbstractListModel(parent), imageManager(imageManager)
{
    connect(imageManager, &Core::ImageManager::imageFetched, this,
            [this](const QUrl &url, const QPixmap &pixmap) {
                auto values = pendingRequests.values(url);
                for (const auto &index : values) {
                    if (index.isValid())
                        emit dataChanged(index, index, { Qt::DecorationRole });
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
        if (!avatarUrlResolver)
            return imageManager->placeholder();
        auto key = avatarUrlResolver(msg.author.get());
        if (imageManager->isCached(key)) {
            return imageManager->get(key);
        } else {
            pendingRequests.insert(key, QPersistentModelIndex(index));
            imageManager->request(key);
            return imageManager->placeholder();
        }
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
