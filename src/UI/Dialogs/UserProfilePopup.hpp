#pragma once

#include <optional>

#include <QDialog>
#include <QPointer>

#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

class QFrame;
class QLabel;
class QPushButton;
class QTabWidget;
class QTextEdit;
class QVBoxLayout;
class QWidget;

namespace Acheron {
namespace Core {
class ClientInstance;
class ImageManager;
} // namespace Core
namespace UI {

class UserProfilePopup : public QDialog
{
    Q_OBJECT
public:
    UserProfilePopup(Core::ImageManager *images, Core::ClientInstance *instance,
                     Core::Snowflake userId, Core::Snowflake guildId,
                     QWidget *parent = nullptr);
    ~UserProfilePopup() override;

protected:
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void buildUi();
    QWidget *buildHeader();
    QWidget *buildBody();

    void renderFromCachedData();
    void renderFromProfile();
    void renderBannerAndBio();
    void renderBadges();
    void renderConnections();
    void renderMutualServers();
    void renderMutualFriends();
    void renderFriendStatus();
    void applyView();

    void loadCachedNote();
    void saveNote();
    void positionOverParent();
    void requestProfile();

    [[nodiscard]] std::optional<Discord::User> cachedUser() const;
    [[nodiscard]] std::optional<Discord::Member> cachedMember() const;

    QString tagHandle() const;
    QString resolvedDisplayName() const;
    QString resolvedAvatarHash() const;
    Core::Snowflake resolvedGuildAvatarOwner() const;
    int resolvedAccentColor() const;
    bool isBot() const;

private:
    [[nodiscard]] Core::Snowflake accountId() const;

    Core::ImageManager *images = nullptr;
    QPointer<Core::ClientInstance> instance;
    Core::Snowflake userId;
    Core::Snowflake guildId;

    bool guildView = true;
    bool hasProfile = false;
    bool noteLoaded = false;
    bool noteDirty = false;
    Discord::UserProfile profile;

    QWidget *headerWidget = nullptr;
    QLabel *bannerLabel = nullptr;
    QLabel *avatarLabel = nullptr;

    QLabel *displayNameLabel = nullptr;
    QLabel *handleLabel = nullptr;
    QLabel *botBadgeLabel = nullptr;

    QWidget *badgesRow = nullptr;

    QWidget *bioSection = nullptr;
    QLabel *bioLabel = nullptr;

    QPushButton *viewToggle = nullptr;

    QLabel *serverJoinLabel = nullptr;
    QWidget *serverJoinRow = nullptr;
    QLabel *discordJoinLabel = nullptr;
    QLabel *friendsSinceLabel = nullptr;

    QWidget *rolesSection = nullptr;
    QLabel *rolesLabel = nullptr;

    QTabWidget *rightTabs = nullptr;

    QTextEdit *noteEdit = nullptr;
    QVBoxLayout *connectionsLayout = nullptr;

    QWidget *mutualServersTab = nullptr;
    QVBoxLayout *mutualServersLayout = nullptr;

    QWidget *mutualFriendsTab = nullptr;
    QVBoxLayout *mutualFriendsLayout = nullptr;
};

} // namespace UI
} // namespace Acheron
