#pragma once

#include <QHash>
#include <QPointer>
#include <QUrl>
#include <QWidget>

#include "Core/Snowflake.hpp"
#include "Discord/Enums.hpp"
#include "GuildSettingsSection.hpp"

class QDialog;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;

namespace Acheron {
namespace Core {
class ClientInstance;
class ImageManager;
} // namespace Core
namespace UI {

class GuildSettingsPage;

class GuildSettingsWindow : public QWidget
{
    Q_OBJECT
public:
    GuildSettingsWindow(Core::ImageManager *images, Core::ClientInstance *instance, Core::Snowflake guildId, QWidget *parent = nullptr);

    void openSection(GuildSettingsSection section);

signals:
    void linkActivated(const QString &url);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void rebuildNavigation();
    void fillNavigation(const QList<GuildSettingsSection> &sections);
    void updateTitle();
    void onNavigationChanged(QListWidgetItem *current, QListWidgetItem *previous);
    void onGuildPermissionsChanged(Core::Snowflake changedGuildId);
    void forceClose();
    void confirmDeleteServer();

    GuildSettingsPage *pageFor(GuildSettingsSection section);
    [[nodiscard]] GuildSettingsPage *currentPage() const;
    [[nodiscard]] QListWidgetItem *itemFor(GuildSettingsSection section) const;

    QPointer<Core::ClientInstance> instance;
    Core::ImageManager *images;
    Core::Snowflake guildId;

    QListWidget *navigation;
    QStackedWidget *stack;
    QHash<GuildSettingsSection, GuildSettingsPage *> pages;
    QUrl iconUrl;
    QPointer<QDialog> discardConfirmation;
    Discord::Permissions shownPermissions;
    bool forcedClose = false;
};

} // namespace UI
} // namespace Acheron
