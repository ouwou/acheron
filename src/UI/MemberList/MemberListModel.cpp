#include "MemberListModel.hpp"

#include "Core/Presence/ActivityFormat.hpp"
#include "Discord/CdnUrls.hpp"

namespace Acheron {
namespace UI {

constexpr static QSize AvatarRequestSize = QSize(32, 32);
constexpr static QSize EmojiRequestSize = QSize(16, 16);

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
    case AvatarRole:
        if (item->type != Core::MemberListItem::Type::Member)
            return QVariant();

        return cachedImage(Discord::Cdn::userAvatar(item->userId,
                                                    item->member.user->avatar.get(),
                                                    AvatarRequestSize.width()),
                           AvatarRequestSize, index);
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
    case PresenceBadgeRole:
    case HasActivityRole:
    case ActivityRole:
        return presenceData(*item, role, index);
    default:
        return {};
    }
}

QVariant MemberListModel::presenceData(const Core::MemberListItem &item, int role,
                                       const QModelIndex &index) const
{
    const bool member = item.type == Core::MemberListItem::Type::Member && presenceManager;
    const Core::Snowflake guildId = manager->currentGuildId();

    if (role == PresenceBadgeRole)
        return QVariant::fromValue(member ? presenceManager->badge(item.userId, guildId)
                                          : Core::PresenceBadge());

    if (role == HasActivityRole)
        return member && !activityText(item, guildId).isEmpty();

    return QVariant::fromValue(member ? activityLine(item, guildId, index) : MemberActivity());
}

QString MemberListModel::activityText(const Core::MemberListItem &item, Core::Snowflake guildId) const
{
    const Discord::Activity *activity = presenceManager->primaryActivity(item.userId, guildId);
    if (activity == nullptr)
        return {};

    return Core::ActivityFormat::secondaryText(*activity);
}

MemberActivity MemberListModel::activityLine(const Core::MemberListItem &item, Core::Snowflake guildId, const QModelIndex &index) const
{
    const Discord::Activity *activity = presenceManager->primaryActivity(item.userId, guildId);
    if (activity == nullptr)
        return {};

    MemberActivity line;
    line.text = Core::ActivityFormat::secondaryText(*activity);
    line.kind = activity->kind();

    if (!activity->isCustom() || !activity->emoji.hasValue())
        return line;

    const Discord::ActivityEmoji &emoji = activity->emoji.get();
    if (emoji.isUnicode())
        line.emojiText = emoji.nameText();
    else
        line.emoji = qvariant_cast<QPixmap>(
                cachedImage(emoji.stillImageUrl(EmojiRequestSize.width()),
                            EmojiRequestSize,
                            index));

    return line;
}

QVariant MemberListModel::cachedImage(const QUrl &url, const QSize &size, const QModelIndex &index) const
{
    if (url.isEmpty())
        return {};

    if (imageManager->isCached(url, size))
        return imageManager->get(url, size, accountId);

    imageManager->get(url, size, accountId);
    avatarTracker.track(url, index);

    return {};
}

void MemberListModel::setPresenceManager(Core::PresenceManager *presences)
{
    if (presenceManager)
        disconnect(presenceManager, nullptr, this, nullptr);

    presenceManager = presences;

    if (presenceManager)
        connect(presenceManager, &Core::PresenceManager::presencesChanged, this, &MemberListModel::onPresencesChanged);
}

void MemberListModel::setAccount(Core::Snowflake id)
{
    accountId = id;
}

void MemberListModel::notifyRows(const QList<int> &rows)
{
    if (rows.isEmpty())
        return;

    for (int row : rows) {
        if (row >= 0 && row < rowCount())
            emit dataChanged(index(row), index(row));
    }
}

void MemberListModel::onItemsChanged(const QList<int> &indices)
{
    notifyRows(indices);
}

void MemberListModel::onPresencesChanged(const QList<Core::Snowflake> &userIds)
{
    if (!manager)
        return;

    notifyRows(manager->indicesForUsers(QSet<Core::Snowflake>(userIds.cbegin(), userIds.cend())));
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

    connect(manager, &Core::MemberListManager::listAboutToReset, this, &MemberListModel::onListAboutToReset);
    connect(manager, &Core::MemberListManager::listReset, this, &MemberListModel::onListReset);
    connect(manager, &Core::MemberListManager::itemsChanged, this, &MemberListModel::onItemsChanged);
}

void MemberListModel::disconnectManager()
{
    if (!manager)
        return;

    disconnect(manager, nullptr, this, nullptr);
}

} // namespace UI
} // namespace Acheron
