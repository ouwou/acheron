#pragma once
#include <QtWidgets>

#include <Core/Snowflake.hpp>
#include "Input/MessageInput.hpp"
#include "MemberList/MemberListView.hpp"
#include "MemberList/MemberListModel.hpp"
#include "MemberList/MemberListDelegate.hpp"
#include "Discord/Entities.hpp"

namespace Acheron {
namespace Core {
class Session;
class ClientInstance;
class TypingTracker;
} // namespace Core
namespace Discord {
struct TypingStart;
}
namespace UI {
class ChatView;
class ChatModel;
class ChannelTreeModel;
class ChannelFilterProxyModel;
class AccountsWindow;
class AccountsModel;
class ChannelTreeView;
struct ChannelNode;
class TypingIndicator;
class SlowModeIndicator;
class ConnectionBanner;
class VoiceStatusBar;
class TabBar;
struct TabEntry;
} // namespace UI
} // namespace Acheron

namespace Acheron {
namespace UI {

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(Core::Session *session, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onChannelSelectionChanged(const QModelIndex &current, const QModelIndex &previous);
    void onTypingStart(const Discord::TypingStart &event);
    void onChannelPermissionsChanged(Core::Snowflake channelId);

private:
    void switchActiveInstance(Core::ClientInstance *instance);
    void setupPermanentConnections(Core::ClientInstance *instance);
    void switchToTabEntry(const TabEntry &entry);
    void activateChannel(const TabEntry &entry);
    void refreshTabReadStates();
    QColor resolveRoleColor(Core::Snowflake userId, Core::Snowflake guildId);
    void updateVoiceStatusLabel();

private:
    void setupUi();
    void setupMenu();

    ChatView *chatView;
    ChatModel *chatModel;

    ChannelTreeView *channelTree;
    ChannelTreeModel *channelTreeModel;
    ChannelFilterProxyModel *channelFilterProxy;

    AccountsModel *accountsModel;

    TabBar *tabBar;
    MessageInput *messageInput;
    TypingIndicator *typingIndicator;
    SlowModeIndicator *slowModeIndicator;
    ConnectionBanner *connectionBanner;
    MemberListView *memberListView;
    MemberListModel *memberListModel;
    Core::TypingTracker *typingTracker;

    VoiceStatusBar *voiceStatusBar;
    AccountsWindow *accountsWindow = nullptr;

private slots:
    void openAccountsWindow();

private:
    Core::Session *session;
    Core::ClientInstance *currentInstance = nullptr;

    Core::Snowflake cachedGuildId = Core::Snowflake::Invalid;
    QHash<Core::Snowflake, QList<Discord::Role>> guildRolesCache;
    QHash<Core::Snowflake, QColor> userColorCache; // current guild

    QSet<Core::Snowflake> instancesSignalsConnected;
    QSplitter *mainSplitter = nullptr;
};

} // namespace UI
} // namespace Acheron