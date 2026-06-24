#pragma once
#include <QtWidgets>

#include <functional>
#include <optional>

#include <Core/Snowflake.hpp>
#include "Input/MessageInput.hpp"
#include "MemberList/MemberListView.hpp"
#include "MemberList/MemberListModel.hpp"
#include "MemberList/MemberListDelegate.hpp"
#include "TabBar/TabBar.hpp"
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
class SettingsWindow;
class ChannelTreeView;
struct ChannelNode;
class TypingIndicator;
class SlowModeIndicator;
class ConnectionBanner;
#ifndef ACHERON_NO_VOICE
class VoiceStatusBar;
#endif
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

    void saveWindowState();
    void restoreWindowState();
    void applyTreeState();
    void captureTreeState(QStringList &expanded,
                          QStringList &collapsed,
                          QStringList &collapsedCategories,
                          QSet<QString> &presentAccounts) const;
    void forEachSourceNode(const std::function<void(const QModelIndex &, ChannelNode *)> &fn) const;
    QString treeNodeKey(const ChannelNode *node) const;
    void maybeActivatePendingChannel(Core::Snowflake accountId);
    QColor resolveRoleColor(Core::Snowflake userId, Core::Snowflake guildId);
    void refreshGuildRoleData(Core::Snowflake guildId);
    void showUserContextMenu(Core::Snowflake userId, Core::Snowflake guildId, QPoint globalPos);
    void selectChannelInTree(Core::Snowflake channelId);
#ifndef ACHERON_NO_VOICE
    void updateVoiceStatusLabel();
#endif

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

#ifndef ACHERON_NO_VOICE
    VoiceStatusBar *voiceStatusBar;
#endif
    AccountsWindow *accountsWindow = nullptr;
    SettingsWindow *settingsWindow = nullptr;

private slots:
    void openAccountsWindow();
    void openSettingsWindow();

private:
    Core::Session *session;
    Core::ClientInstance *currentInstance = nullptr;

    Core::Snowflake cachedGuildId = Core::Snowflake::Invalid;
    QHash<Core::Snowflake, QColor> userColorCache; // current guild

    QSet<Core::Snowflake> instancesSignalsConnected;
    QSplitter *mainSplitter = nullptr;

    // restored-but-not-yet-activated channel. gotta wait for READY
    std::optional<TabEntry> pendingActiveEntry;
    // same thing for expansion state
    bool hasSavedTreeState = false;
    QSet<QString> savedExpandedNodes;
    QSet<QString> savedCollapsedNodes;
    QSet<QString> savedCollapsedCategories;
};

} // namespace UI
} // namespace Acheron