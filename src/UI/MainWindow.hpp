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
class ForumBrowser;
class ForumPostModel;
class ThreadBrowserPopup;
class ChannelTreeModel;
class ChannelFilterProxyModel;
class AccountsWindow;
class AccountsModel;
class SettingsWindow;
class ChannelTreeView;
class ServerRailView;
class ServerRailModel;
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
    void openForumPost(Core::Snowflake threadId, Core::Snowflake guildId);
    void onNewPostRequested();
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
    void confirmAndLeaveGuild(Core::Snowflake accountId, Core::Snowflake guildId);
    void showUserContextMenu(Core::Snowflake userId, Core::Snowflake guildId, QPoint globalPos);
    void selectChannelInTree(Core::Snowflake channelId);

public:
    enum class ChannelListMode {
        Tree,
        Classic
    };
    void setChannelListMode(ChannelListMode mode);

private:
    QWidget *buildLeftSide();
    void onRailAccountHomeSelected(Core::Snowflake accountId);
    void onRailAccountHomeClicked(Core::Snowflake accountId);
    void onRailGuildSelected(Core::Snowflake accountId, Core::Snowflake guildId);
    void onRailGuildClicked(Core::Snowflake accountId, Core::Snowflake guildId);
    void selectInitialRailItem();
    void applyPendingRailSelection(Core::Snowflake accountId);
    Core::Snowflake resolveRailChannel(Core::Snowflake accountId, Core::Snowflake guildId);
    Core::Snowflake firstReadableChannel(Core::Snowflake accountId, Core::Snowflake guildId);
    bool channelReadable(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId);
    void markIndexAsRead(Core::Snowflake accountId, const QModelIndex &sourceIndex);
    void recordLastViewedChannel(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId);
#ifndef ACHERON_NO_VOICE
    void updateVoiceStatusLabel();
#endif

private:
    void setupUi();
    void setupMenu();

    [[nodiscard]] TabEntry makeTabEntry(ChannelNode *node, ChannelNode *accountNode) const;

    enum class ViewMode { TextChannel,
                          ForumBrowse,
                          ForumSplit,
                          ThreadPopout };
    void setViewMode(ViewMode mode);
    void setMemberListVisible(bool visible);
    void updateMemberListVisibility();
    void openForumChannel(Core::ClientInstance *instance, Core::Snowflake forumId, Core::Snowflake guildId);
    void applyChannelChrome(Core::ClientInstance *instance, Core::Snowflake channelId, const QString &name, bool isDm, Core::Snowflake guildId);
    void switchChatChannel(Core::Snowflake channelId, Core::Snowflake guildId);

    void openThreadBrowser();
    void navigateToChannel(Core::Snowflake channelId);
    void setThreadBrowserTarget(Core::Snowflake channelId);

    ChatView *chatView;
    ChatModel *chatModel;

    // forum/split state
    ViewMode viewMode = ViewMode::TextChannel;
    bool memberListWanted = false;
    QSplitter *centerSplitter = nullptr;
    QWidget *threadPane = nullptr;
    QWidget *threadHeader = nullptr;
    QPushButton *popOutButton = nullptr;
    QPushButton *closeThreadButton = nullptr;
    ForumBrowser *forumBrowser = nullptr;
    ForumPostModel *forumModel = nullptr;
    Core::Snowflake currentForumId;
    Core::Snowflake currentForumGuildId;

    ChannelTreeView *channelTree;
    ChannelTreeModel *channelTreeModel;
    ChannelFilterProxyModel *channelFilterProxy;

    ChannelListMode channelListMode = ChannelListMode::Tree;
    QWidget *leftSideWidget = nullptr;
    ServerRailView *serverRail = nullptr;
    ServerRailModel *serverRailModel = nullptr;
    QLabel *guildHeaderLabel = nullptr;

    AccountsModel *accountsModel;

    TabBar *tabBar;
    QWidget *channelToolbar = nullptr;
    QToolButton *threadBrowserButton = nullptr;
    ThreadBrowserPopup *threadBrowser = nullptr;
    Core::Snowflake threadBrowserChannelId = Core::Snowflake::Invalid;
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

    bool railHasSelection = false;
    bool railSelectedIsHome = false;
    Core::Snowflake railSelectedAccountId;
    Core::Snowflake railSelectedGuildId;
    // keyed "accountId:guildId"
    QHash<QString, Core::Snowflake> lastViewedChannel;
    // also gotta wait for READY here
    bool hasSavedRailSelection = false;
    bool savedRailIsHome = true;
    Core::Snowflake savedRailAccountId;
    Core::Snowflake savedRailGuildId;
};

} // namespace UI
} // namespace Acheron