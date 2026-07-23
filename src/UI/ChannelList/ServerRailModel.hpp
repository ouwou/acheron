#pragma once

#include <QAbstractListModel>
#include <QPersistentModelIndex>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVector>

#include <optional>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {
class Session;
struct AccountInfo;
} // namespace Core
namespace UI {

class ChannelTreeModel;

// piggyback off the ChannelTreeModel
class ServerRailModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum class Kind {
        AccountHome,
        Folder,
        Server,
    };

    enum Roles {
        KindRole = Qt::UserRole,
        AccountIdRole,
        IdRole,
        IsUnreadRole,
        MentionCountRole,
        IsMutedRole,
        IsExpandedRole,
        IsSelectedRole,
        IsFirstOfAccountRole,
        DepthRole,
        FolderChildIconsRole,
        FolderColorRole,
        InFolderGroupRole, // true for expanded folder and its child guilds for painting purposes
        OwnerIdRole,
    };

    ServerRailModel(Core::Session *session, ChannelTreeModel *source, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    void toggleFolder(Core::Snowflake accountId, Core::Snowflake folderId);
    [[nodiscard]] bool isFolderExpanded(Core::Snowflake accountId, Core::Snowflake folderId) const;

    void setSelected(Kind kind, Core::Snowflake accountId, Core::Snowflake id);

    [[nodiscard]] QStringList expandedFolderKeys() const;
    void setExpandedFolderKeys(const QStringList &keys);

    void rebuild();

private:
    struct RailEntry
    {
        Kind kind;
        Core::Snowflake accountId;
        Core::Snowflake id;
        QString name;
        bool isFirstOfAccount = false;
        int depth = 0;
        bool inFolderGroup = false;
        std::optional<quint64> folderColor;
        QString accountAvatarHash;
        QVariantList folderIcons;
        QPersistentModelIndex srcIndex;
        QPersistentModelIndex unreadSrcIndex; // distinct because account maps to DMHeader
    };

    static QString folderKey(Core::Snowflake accountId, Core::Snowflake folderId);
    QModelIndex dmHeaderFor(const QModelIndex &accountIndex) const;
    Core::Snowflake folderForGuild(Core::Snowflake accountId, Core::Snowflake guildId) const;
    QVariant decorationFor(const RailEntry &entry) const;
    QVariantList computeFolderChildIcons(const QModelIndex &folderIndex) const;
    void emitAllChanged();
    // so we dont rebuild for unrelated changes
    [[nodiscard]] bool structureAffectsRail(const QModelIndex &parent) const;
    void onSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QVector<int> &roles);

    Core::Session *session;
    ChannelTreeModel *source;

    QVector<RailEntry> entries;
    QSet<QString> expandedFolders;

    Kind selectedKind = Kind::AccountHome;
    Core::Snowflake selectedAccountId;
    Core::Snowflake selectedGuildId;
    Core::Snowflake selectedFolderId;

    mutable QSet<QUrl> pendingAccountAvatars;
};

} // namespace UI
} // namespace Acheron
