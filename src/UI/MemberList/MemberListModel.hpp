#pragma once

#include <QAbstractListModel>
#include <QPixmap>
#include <QPointer>

#include "Core/MemberListManager.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "UI/AvatarRequestTracker.hpp"

namespace Acheron {
namespace UI {

struct MemberActivity
{
    QString text;
    Discord::ActivityType kind = Discord::ActivityType::PLAYING;
    QPixmap emoji;
    QString emojiText;
};

class MemberListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        ItemTypeRole = Qt::UserRole + 1,
        UserIdRole,
        UsernameRole,
        AvatarRole,
        RoleColorRole,
        GroupNameRole,
        GroupCountRole,
        GroupColorRole,
        LoadedRole,
        PresenceBadgeRole,
        HasActivityRole,
        ActivityRole,
    };

    explicit MemberListModel(Core::ImageManager *imageManager, QObject *parent = nullptr);

    void setManager(Core::MemberListManager *manager);
    void setPresenceManager(Core::PresenceManager *presences);
    void setAccount(Core::Snowflake accountId);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;

private:
    QVariant presenceData(const Core::MemberListItem &item, int role, const QModelIndex &index) const;
    QString activityText(const Core::MemberListItem &item, Core::Snowflake guildId) const;
    MemberActivity activityLine(const Core::MemberListItem &item, Core::Snowflake guildId, const QModelIndex &index) const;
    QVariant cachedImage(const QUrl &url, const QSize &size, const QModelIndex &index) const;

    void onListAboutToReset();
    void onListReset();
    void onItemsChanged(const QList<int> &indices);
    void onPresencesChanged(const QList<Core::Snowflake> &userIds);
    void onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);

    void connectManager();
    void disconnectManager();

    void notifyRows(const QList<int> &rows);

    QPointer<Core::MemberListManager> manager;
    QPointer<Core::PresenceManager> presenceManager;
    Core::ImageManager *imageManager;
    Core::Snowflake accountId;

    mutable AvatarRequestTracker<QPersistentModelIndex> avatarTracker;
};

} // namespace UI
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::UI::MemberActivity)
