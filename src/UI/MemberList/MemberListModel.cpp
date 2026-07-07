#include "MemberListModel.hpp"

#include "Discord/CdnUrls.hpp"

namespace Acheron {
namespace UI {

constexpr static QSize AvatarRequestSize = QSize(32, 32);

MemberListModel::MemberListModel(Core::ImageManager *imageManager, QObject *parent)
    : QAbstractListModel(parent), imageManager(imageManager)
{
    connect(imageManager, &Core::ImageManager::imageFetched, this, &MemberListModel::onImageFetched);
}

void MemberListModel::setManager(Core::MemberListManager *newManager)
{
    beginResetModel();
    disconnectManager();
    manager = newManager;
    avatarTracker.clear();
    connectManager();
    endResetModel();
}

int MemberListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !manager)
        return 0;

    return manager->totalItemCount();
}

QVariant MemberListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !manager)
        return {};

    int row = index.row();
    if (row < 0 || row >= manager->totalItemCount())
        return {};

    const auto *item = manager->itemAt(row);

    if (!item) {
        switch (role) {
        case ItemTypeRole:
            return static_cast<int>(Core::MemberListItem::Type::Placeholder);
        case LoadedRole:
            return false;
        default:
            return {};
        }
    }

    switch (role) {
    case ItemTypeRole:
        return static_cast<int>(item->type);
    case LoadedRole:
        return true;
    case UserIdRole:
        return item->type == Core::MemberListItem::Type::Member
                       ? QVariant::fromValue(static_cast<quint64>(item->userId))
                       : QVariant();
    case UsernameRole:
        return item->type == Core::MemberListItem::Type::Member
                       ? item->displayName
                       : QString();
    case AvatarRole: {
        if (item->type != Core::MemberListItem::Type::Member)
            return QVariant();

        Core::Snowflake userId = item->userId;
        QUrl url = Discord::Cdn::userAvatar(userId, item->member.user->avatar.get(), AvatarRequestSize.width());
        if (url.isEmpty())
            return QVariant();

        if (imageManager->isCached(url, AvatarRequestSize))
            return imageManager->get(url, AvatarRequestSize);

        imageManager->get(url, AvatarRequestSize);
        avatarTracker.track(url, index);

        return QVariant();
    }
    case RoleColorRole:
        return item->type == Core::MemberListItem::Type::Member
                       ? QVariant::fromValue(item->roleColor)
                       : QVariant();
    case GroupNameRole:
        return item->type == Core::MemberListItem::Type::Group
                       ? item->groupName
                       : QString();
    case GroupCountRole:
        return item->type == Core::MemberListItem::Type::Group
                       ? item->groupCount
                       : 0;
    case GroupColorRole:
        return item->type == Core::MemberListItem::Type::Group
                       ? QVariant::fromValue(item->groupColor)
                       : QVariant();
    }

    return {};
}

void MemberListModel::onListAboutToReset()
{
    beginResetModel();
}

void MemberListModel::onListReset()
{
    avatarTracker.clear();
    endResetModel();
}

void MemberListModel::onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap)
{
    Q_UNUSED(size);
    Q_UNUSED(pixmap);

    avatarTracker.notify(url, [this](const QModelIndex &index) {
        if (index.isValid())
            emit dataChanged(index, index);
    });
}

void MemberListModel::connectManager()
{
    if (!manager)
        return;

    connect(manager, &Core::MemberListManager::listAboutToReset,
            this, &MemberListModel::onListAboutToReset);
    connect(manager, &Core::MemberListManager::listReset,
            this, &MemberListModel::onListReset);
}

void MemberListModel::disconnectManager()
{
    if (!manager)
        return;

    disconnect(manager, nullptr, this, nullptr);
}

} // namespace UI
} // namespace Acheron
