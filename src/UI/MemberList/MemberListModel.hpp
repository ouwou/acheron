#pragma once

#include <QAbstractListModel>

#include "Core/MemberListManager.hpp"
#include "Core/ImageManager.hpp"

namespace Acheron {
namespace UI {

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
    };

    explicit MemberListModel(Core::ImageManager *imageManager, QObject *parent = nullptr);

    void setManager(Core::MemberListManager *manager);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;

private:
    void onListAboutToReset();
    void onListReset();
    void onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);

    void connectManager();
    void disconnectManager();

    Core::MemberListManager *manager = nullptr;
    Core::ImageManager *imageManager;

    mutable QHash<QUrl, QList<int>> pendingAvatars;
};

} // namespace UI
} // namespace Acheron
