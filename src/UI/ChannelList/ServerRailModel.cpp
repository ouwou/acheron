#include "ServerRailModel.hpp"

#include "ChannelNode.hpp"
#include "ChannelTreeModel.hpp"

#include "Core/AccountInfo.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Session.hpp"
#include "Discord/CdnUrls.hpp"

#include <QPixmap>

namespace Acheron {
namespace UI {

using Core::Snowflake;

namespace {
ChannelNode::Type typeOf(const QModelIndex &idx)
{
    return static_cast<ChannelNode::Type>(idx.data(ChannelTreeModel::TypeRole).toInt());
}
Snowflake idOf(const QModelIndex &idx)
{
    return Snowflake(idx.data(ChannelTreeModel::IdRole).toULongLong());
}
} // namespace

ServerRailModel::ServerRailModel(Core::Session *session, ChannelTreeModel *source, QObject *parent)
    : QAbstractListModel(parent), session(session), source(source)
{
    rebuild();

    connect(source, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &parent, int, int) {
                if (structureAffectsRail(parent))
                    rebuild();
            });
    connect(source, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex &parent, int, int) {
                if (structureAffectsRail(parent))
                    rebuild();
            });
    connect(source, &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex &src, int, int, const QModelIndex &dst, int) {
                if (structureAffectsRail(src) || structureAffectsRail(dst))
                    rebuild();
            });
    connect(source, &QAbstractItemModel::modelReset, this, [this]() { rebuild(); });

    connect(source, &QAbstractItemModel::dataChanged, this, &ServerRailModel::onSourceDataChanged);

    connect(session, &Core::Session::accountDetailsUpdated, this,
            [this](const Core::AccountInfo &info) {
                bool changed = false;
                for (auto &e : entries) {
                    if (e.kind == Kind::AccountHome && e.accountId == info.id) {
                        e.name = info.displayName.isEmpty() ? info.username : info.displayName;
                        e.accountAvatarHash = info.avatar;
                        changed = true;
                    }
                }
                if (changed)
                    emitAllChanged();
            });

    connect(session->getImageManager(), &Core::ImageManager::imageFetched, this,
            [this](const QUrl &url, const QSize &, const QPixmap &) {
                if (pendingAccountAvatars.remove(url))
                    emitAllChanged();
            });
}

int ServerRailModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return entries.size();
}

QVariant ServerRailModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= entries.size())
        return {};

    const RailEntry &e = entries[index.row()];

    switch (role) {
    case KindRole:
        return static_cast<int>(e.kind);
    case AccountIdRole:
        return static_cast<quint64>(e.accountId);
    case IdRole:
        return static_cast<quint64>(e.id);
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
        return e.name;
    case IsFirstOfAccountRole:
        return e.isFirstOfAccount;
    case DepthRole:
        return e.depth;
    case IsUnreadRole:
        return e.unreadSrcIndex.isValid()
                       ? e.unreadSrcIndex.data(ChannelTreeModel::CountsForGuildUnreadRole)
                       : QVariant(false);
    case MentionCountRole:
        return e.unreadSrcIndex.isValid()
                       ? e.unreadSrcIndex.data(ChannelTreeModel::MentionCountRole)
                       : QVariant(0);
    case IsMutedRole:
        return e.unreadSrcIndex.isValid()
                       ? e.unreadSrcIndex.data(ChannelTreeModel::IsMutedRole)
                       : QVariant(false);
    case IsExpandedRole:
        return e.kind == Kind::Folder && expandedFolders.contains(folderKey(e.accountId, e.id));
    case IsSelectedRole:
        if (e.kind == Kind::AccountHome)
            return selectedKind == Kind::AccountHome && e.accountId == selectedAccountId;
        if (e.kind == Kind::Server)
            return selectedKind == Kind::Server && e.accountId == selectedAccountId &&
                   e.id == selectedGuildId;
        // folder is considered selected if collapsed and contains a selected server
        return selectedKind == Kind::Server && e.accountId == selectedAccountId &&
               e.id == selectedFolderId && !expandedFolders.contains(folderKey(e.accountId, e.id));
    case InFolderGroupRole:
        return e.inFolderGroup;
    case FolderColorRole:
        return e.folderColor.has_value() ? QVariant(static_cast<quint64>(*e.folderColor))
                                         : QVariant();
    case FolderChildIconsRole:
        return e.kind == Kind::Folder ? QVariant(e.folderIcons) : QVariant();
    case OwnerIdRole:
        if (e.kind == Kind::Server && e.srcIndex.isValid())
            return e.srcIndex.data(ChannelTreeModel::OwnerIdRole);
        return {};
    case Qt::DecorationRole:
        return decorationFor(e);
    default:
        return {};
    }
}

QVariant ServerRailModel::decorationFor(const RailEntry &entry) const
{
    if (entry.kind == Kind::Server) {
        if (entry.srcIndex.data(ChannelTreeModel::IconHashRole).toString().isEmpty())
            return {};
        return source->data(entry.srcIndex, Qt::DecorationRole);
    }

    if (entry.kind == Kind::AccountHome) {
        if (entry.accountAvatarHash.isEmpty())
            return {};
        const QSize size(64, 64);
        QUrl url = Discord::Cdn::userAvatar(entry.accountId, entry.accountAvatarHash, size.width());
        if (url.isEmpty())
            return {};
        QPixmap pixmap = session->getImageManager()->get(url, size, Core::PinGroup::ChannelList);
        if (!session->getImageManager()->isCached(url, size)) {
            pendingAccountAvatars.insert(url);
            return {};
        }
        return pixmap;
    }

    return {};
}

QVariantList ServerRailModel::computeFolderChildIcons(const QModelIndex &folderIndex) const
{
    QVariantList icons;
    int n = source->rowCount(folderIndex);
    for (int i = 0; i < n && icons.size() < 4; ++i) {
        QModelIndex child = source->index(i, 0, folderIndex);
        if (typeOf(child) != ChannelNode::Type::Server)
            continue;
        if (child.data(ChannelTreeModel::IconHashRole).toString().isEmpty())
            icons.append(QVariant::fromValue(QPixmap()));
        else
            icons.append(source->data(child, Qt::DecorationRole));
    }
    return icons;
}

QModelIndex ServerRailModel::dmHeaderFor(const QModelIndex &accountIndex) const
{
    int n = source->rowCount(accountIndex);
    for (int i = 0; i < n; ++i) {
        QModelIndex c = source->index(i, 0, accountIndex);
        if (typeOf(c) == ChannelNode::Type::DMHeader)
            return c;
    }
    return {};
}

void ServerRailModel::rebuild()
{
    beginResetModel();
    entries.clear();

    int accountRows = source->rowCount({});
    for (int a = 0; a < accountRows; ++a) {
        QModelIndex accIdx = source->index(a, 0, {});
        if (typeOf(accIdx) != ChannelNode::Type::Account)
            continue;

        Snowflake accountId = idOf(accIdx);

        RailEntry home;
        home.kind = Kind::AccountHome;
        home.accountId = accountId;
        home.id = accountId;
        home.name = accIdx.data(Qt::DisplayRole).toString();
        home.isFirstOfAccount = true;
        home.srcIndex = accIdx;
        home.unreadSrcIndex = dmHeaderFor(accIdx);
        home.accountAvatarHash = session->getAccountInfo(accountId).avatar;
        entries.push_back(home);

        int childRows = source->rowCount(accIdx);
        for (int c = 0; c < childRows; ++c) {
            QModelIndex childIdx = source->index(c, 0, accIdx);
            ChannelNode::Type t = typeOf(childIdx);

            if (t == ChannelNode::Type::Server) {
                RailEntry e;
                e.kind = Kind::Server;
                e.accountId = accountId;
                e.id = idOf(childIdx);
                e.name = childIdx.data(Qt::DisplayRole).toString();
                e.srcIndex = childIdx;
                e.unreadSrcIndex = childIdx;
                entries.push_back(e);
            } else if (t == ChannelNode::Type::Folder) {
                Snowflake folderId = idOf(childIdx);
                bool expanded = expandedFolders.contains(folderKey(accountId, folderId));

                std::optional<quint64> folderColor;
                QVariant fcVar = childIdx.data(ChannelTreeModel::FolderColorRole);
                if (fcVar.isValid())
                    folderColor = fcVar.toULongLong();

                RailEntry f;
                f.kind = Kind::Folder;
                f.accountId = accountId;
                f.id = folderId;
                f.name = childIdx.data(Qt::DisplayRole).toString();
                f.srcIndex = childIdx;
                f.unreadSrcIndex = childIdx;
                f.folderColor = folderColor;
                f.inFolderGroup = expanded; // group background only when expanded
                f.folderIcons = computeFolderChildIcons(childIdx);
                entries.push_back(f);

                if (expanded) {
                    int gcount = source->rowCount(childIdx);
                    for (int g = 0; g < gcount; ++g) {
                        QModelIndex gIdx = source->index(g, 0, childIdx);
                        if (typeOf(gIdx) != ChannelNode::Type::Server)
                            continue;
                        RailEntry e;
                        e.kind = Kind::Server;
                        e.accountId = accountId;
                        e.id = idOf(gIdx);
                        e.name = gIdx.data(Qt::DisplayRole).toString();
                        e.depth = 1;
                        e.srcIndex = gIdx;
                        e.unreadSrcIndex = gIdx;
                        e.folderColor = folderColor;
                        e.inFolderGroup = true;
                        entries.push_back(e);
                    }
                }
            }
        }
    }

    endResetModel();
}

void ServerRailModel::toggleFolder(Snowflake accountId, Snowflake folderId)
{
    QString key = folderKey(accountId, folderId);
    if (expandedFolders.contains(key))
        expandedFolders.remove(key);
    else
        expandedFolders.insert(key);
    rebuild();
}

bool ServerRailModel::isFolderExpanded(Snowflake accountId, Snowflake folderId) const
{
    return expandedFolders.contains(folderKey(accountId, folderId));
}

void ServerRailModel::setSelected(Kind kind, Snowflake accountId, Snowflake id)
{
    Snowflake newGuild = (kind == Kind::Server) ? id : Snowflake();
    if (selectedKind == kind && selectedAccountId == accountId && selectedGuildId == newGuild)
        return;
    selectedKind = kind;
    selectedAccountId = accountId;
    selectedGuildId = newGuild;
    selectedFolderId = (kind == Kind::Server) ? folderForGuild(accountId, id) : Snowflake();
    emitAllChanged();
}

Snowflake ServerRailModel::folderForGuild(Snowflake accountId, Snowflake guildId) const
{
    int accountRows = source->rowCount({});
    for (int a = 0; a < accountRows; ++a) {
        QModelIndex accIdx = source->index(a, 0, {});
        if (typeOf(accIdx) != ChannelNode::Type::Account || idOf(accIdx) != accountId)
            continue;
        int childRows = source->rowCount(accIdx);
        for (int c = 0; c < childRows; ++c) {
            QModelIndex childIdx = source->index(c, 0, accIdx);
            if (typeOf(childIdx) != ChannelNode::Type::Folder)
                continue;
            int gcount = source->rowCount(childIdx);
            for (int g = 0; g < gcount; ++g) {
                QModelIndex gIdx = source->index(g, 0, childIdx);
                if (typeOf(gIdx) == ChannelNode::Type::Server && idOf(gIdx) == guildId)
                    return idOf(childIdx);
            }
        }
        break;
    }
    return {};
}

QStringList ServerRailModel::expandedFolderKeys() const
{
    return QStringList(expandedFolders.cbegin(), expandedFolders.cend());
}

void ServerRailModel::setExpandedFolderKeys(const QStringList &keys)
{
    expandedFolders = QSet<QString>(keys.cbegin(), keys.cend());
    rebuild();
}

QString ServerRailModel::folderKey(Snowflake accountId, Snowflake folderId)
{
    return QStringLiteral("%1:%2")
            .arg(static_cast<quint64>(accountId))
            .arg(static_cast<quint64>(folderId));
}

void ServerRailModel::emitAllChanged()
{
    if (entries.isEmpty())
        return;
    emit dataChanged(index(0, 0), index(entries.size() - 1, 0));
}

bool ServerRailModel::structureAffectsRail(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return true; // account at root
    auto t = static_cast<ChannelNode::Type>(parent.data(ChannelTreeModel::TypeRole).toInt());
    return t == ChannelNode::Type::Account || t == ChannelNode::Type::Folder;
}

void ServerRailModel::onSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                                          const QVector<int> &)
{
    const QModelIndex parent = topLeft.parent();
    const int first = topLeft.row();
    const int last = bottomRight.row();

    auto inRange = [&](const QPersistentModelIndex &idx) {
        return idx.isValid() && QModelIndex(idx).parent() == parent && idx.row() >= first &&
               idx.row() <= last;
    };

    for (int i = 0; i < entries.size(); ++i) {
        RailEntry &e = entries[i];
        bool affected = inRange(e.srcIndex) || inRange(e.unreadSrcIndex);

        if (e.kind == Kind::Folder && parent == QModelIndex(e.srcIndex)) {
            e.folderIcons = computeFolderChildIcons(e.srcIndex);
            affected = true;
        }

        if (affected)
            emit dataChanged(index(i, 0), index(i, 0));
    }
}

} // namespace UI
} // namespace Acheron
