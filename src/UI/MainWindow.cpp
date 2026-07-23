#include "MainWindow.hpp"

#include <QMessageBox>
#include <QPointer>
#include <QSettings>

#include "Chat/ChatModel.hpp"
#include "Chat/ChatDelegate.hpp"
#include "Chat/ChatView.hpp"
#include "Forum/ForumBrowser.hpp"
#include "Forum/ForumPostModel.hpp"
#include "Forum/NewPostDialog.hpp"
#include "ThreadBrowser/ThreadBrowserPopup.hpp"
#include "Core/ForumManager.hpp"
#include "ChannelList/ChannelTreeModel.hpp"
#include "ChannelList/ChannelFilterProxyModel.hpp"
#include "ChannelList/ChannelDelegate.hpp"
#include "ChannelList/ChannelTreeView.hpp"
#include "ChannelList/ServerRailView.hpp"
#include "ChannelList/ServerRailModel.hpp"
#include "ChannelList/ServerRailDelegate.hpp"
#include "TabBar/TabBar.hpp"
#include "Accounts/AccountsWindow.hpp"
#include "Settings/SettingsWindow.hpp"
#include "Accounts/AccountsModel.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/AccountInfo.hpp"
#include "Core/UserManager.hpp"
#include "Core/TypingTracker.hpp"
#include "Core/Logging.hpp"
#include "Core/ReadStateManager.hpp"
#include "Core/Theme/Icons.hpp"
#include "Discord/Events.hpp"
#include "TypingIndicator.hpp"
#include "SlowModeIndicator.hpp"
#include "ConnectionBanner.hpp"
#include "BrowserCaptchaResolver.hpp"
#include "Dialogs/ConfirmPopup.hpp"
#include "Dialogs/UserProfilePopup.hpp"
#include "Discord/CdnUrls.hpp"
#include "Core/ImageManager.hpp"
#include "Core/MemberListManager.hpp"
#include "Core/Session.hpp"
#include "Core/Theme/Manager.hpp"
#ifndef ACHERON_NO_VOICE
#include "Core/AV/VoiceManager.hpp"
#include "VoiceStatusBar.hpp"
#endif

using namespace Acheron::Core;

namespace Acheron {
namespace UI {

namespace {
QString guildChannelKey(Core::Snowflake accountId, Core::Snowflake guildId)
{
    return QStringLiteral("%1:%2")
            .arg(static_cast<quint64>(accountId))
            .arg(static_cast<quint64>(guildId));
}
} // namespace

MainWindow::MainWindow(Session *session, QWidget *parent) : QMainWindow(parent), session(session)
{
    auto *captchaResolver = new BrowserCaptchaResolver(this, this);
    session->setCaptchaResolver(captchaResolver);

    chatModel = new ChatModel(session->getImageManager(), this);
    channelTreeModel = new ChannelTreeModel(session, this);
    channelFilterProxy = new ChannelFilterProxyModel(session, this);
    channelFilterProxy->setSourceModel(channelTreeModel);
    channelFilterProxy->setDynamicSortFilter(true);
    channelFilterProxy->sort(0);
    accountsModel = new AccountsModel(session, this);
    serverRailModel = new ServerRailModel(session, channelTreeModel, this);

    chatModel->setAvatarUrlResolver([](const Discord::User &user) -> QUrl {
        return Discord::Cdn::userAvatar(user.id.get(), user.avatar.get(), 64);
    });

    chatModel->setDisplayNameResolver([this](Snowflake userId, Snowflake guildId) -> QString {
        if (!currentInstance)
            return QString();
        return currentInstance->users()->getDisplayName(userId, guildId);
    });

    chatModel->setRoleColorResolver(
            [this](Snowflake userId, Snowflake guildId) { return resolveRoleColor(userId, guildId); });

    typingTracker = new TypingTracker(this);
    memberListModel = new MemberListModel(session->getImageManager(), this);

    channelListMode = (QSettings().value("ui/channelListMode").toString() == "classic")
                              ? ChannelListMode::Classic
                              : ChannelListMode::Tree;

    setupUi();
    setupMenu();

    qApp->installEventFilter(this);

    typingIndicator->setRoleColorResolver(
            [this](Snowflake userId, Snowflake guildId) { return resolveRoleColor(userId, guildId); });

    connect(typingTracker, &TypingTracker::typersChanged, this,
            [this]() { typingIndicator->setTypers(typingTracker->getActiveTypers()); });

    connect(session, &Session::ready, this, [this](const Discord::Ready &ready) {
        channelTreeModel->populateFromReady(ready);
        if (channelListMode == ChannelListMode::Tree)
            channelTree->performDefaultExpansion();
        applyTreeState();
        maybeActivatePendingChannel(ready.user->id);
        refreshTabReadStates();
        if (channelListMode == ChannelListMode::Classic)
            applyPendingRailSelection(ready.user->id);
    });

    connect(accountsModel, &AccountsModel::dataChanged, this,
            [this, session](const QModelIndex &topLeft, const QModelIndex &bottomRight,
                            const QVector<int> &roles) {
                for (int row = topLeft.row(); row <= bottomRight.row(); ++row) {
                    QModelIndex idx = accountsModel->index(row, 0);
                    auto accPtr = idx.data(AccountsModel::AccountObjectRole).value<void *>();

                    Acheron::Core::AccountInfo acc =
                            *static_cast<Acheron::Core::AccountInfo *>(accPtr);

                    if (acc.state == Acheron::Core::ConnectionState::Connected) {
                        channelTreeModel->addAccount(acc);

                        ClientInstance *instance = session->client(acc.id);
                        if (instance)
                            setupPermanentConnections(instance);

                        for (int i = 0; i < channelFilterProxy->rowCount(QModelIndex()); ++i) {
                            QModelIndex proxyIdx = channelFilterProxy->index(i, 0, QModelIndex());
                            if (proxyIdx.data(ChannelTreeModel::IdRole).toULongLong() == acc.id) {
                                channelTree->expand(proxyIdx);
                                break;
                            }
                        }
                    } else if (acc.state == Acheron::Core::ConnectionState::Disconnected) {
                        bool wasShowing = channelListMode == ChannelListMode::Classic && railSelectedAccountId == acc.id;
                        channelTreeModel->removeAccount(acc.id);
                        instancesSignalsConnected.remove(acc.id);
                        if (wasShowing) {
                            railHasSelection = false;
                            channelTree->setRootIndex({});
                            selectInitialRailItem();
                        }
                    }
                }
            });

    for (const auto &instance : session->getClients()) {
        if (instance)
            setupPermanentConnections(instance);
    }

    restoreWindowState();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveWindowState();

    hide();

    // if (session)
    //    session->shutdown();

    event->accept();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *ev)
{
    if (ev->type() == QEvent::MouseButtonPress && isActiveWindow()) {
        auto *me = static_cast<QMouseEvent *>(ev);
        if (me->button() == Qt::BackButton) {
            tabBar->navigateBack();
            return true;
        }
        if (me->button() == Qt::ForwardButton) {
            tabBar->navigateForward();
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}

void MainWindow::onChannelSelectionChanged(const QModelIndex &current, const QModelIndex &previous)
{
    if (!current.isValid())
        return;

    QModelIndex sourceIndex = channelFilterProxy->mapToSource(current);
    auto node = static_cast<ChannelNode *>(sourceIndex.internalPointer());

    if (!node || !node->opensChat())
        return;

    ChannelNode *accountNode = channelTreeModel->getAccountNodeFor(node);
    if (!accountNode) {
        messageInput->setEnabled(false);
        return;
    }

    channelFilterProxy->setSelectedChannel(node->id, accountNode->id);
    channelTree->viewport()->update();

    ClientInstance *selectedInstance = session->client(accountNode->id);
    if (!selectedInstance) {
        messageInput->setEnabled(false);
        return;
    }

    if (selectedInstance != currentInstance)
        switchActiveInstance(selectedInstance);

    channelTreeModel->clearTemporaryThread(node->id);

    ChannelNode *forumNode = node->type == ChannelNode::Type::Thread ? node->parent : node;

    if (forumNode && forumNode->type == ChannelNode::Type::Forum) {
        ChannelNode *gNode = ChannelTreeModel::findGuildNode(node);
        Snowflake gId = gNode ? gNode->id : Snowflake::Invalid;

        setThreadBrowserTarget(Snowflake::Invalid);
        openForumChannel(selectedInstance, forumNode->id, gId);
        tabBar->updateCurrentTab(makeTabEntry(forumNode, accountNode));

        if (gNode)
            recordLastViewedChannel(accountNode->id, gNode->id, forumNode->id);

        if (node->type == ChannelNode::Type::Thread)
            openForumPost(node->id, gId);
        return;
    }

    selectedInstance->readState()->setActiveChannel(node->id);
    setViewMode(ViewMode::TextChannel);

    ChannelNode *guildNode = ChannelTreeModel::findGuildNode(node);
    Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;
    bool isDm = node->type == ChannelNode::Type::DMChannel;

    applyChannelChrome(selectedInstance, node->id, node->name, isDm, guildId);
    switchChatChannel(node->id, guildId);
    selectedInstance->messages()->requestLoadChannel(node->id);

    if (node->isUnread && node->lastMessageId.isValid())
        selectedInstance->readState()->markChannelAsRead(node->id, node->lastMessageId);

    tabBar->updateCurrentTab(makeTabEntry(node, accountNode));

    if (node->type == ChannelNode::Type::Channel && guildNode)
        recordLastViewedChannel(accountNode->id, guildNode->id, node->id);
    else if (isDm)
        recordLastViewedChannel(accountNode->id, Snowflake::Invalid, node->id);
}

void MainWindow::applyChannelChrome(Core::ClientInstance *instance, Core::Snowflake channelId,
                                    const QString &name, bool isDm, Core::Snowflake guildId)
{
    Snowflake userId = instance->accountId();
    messageInput->setMaxUploadSize(instance->discord()->getMaxUploadSize(channelId));

    Snowflake threadParentId = Snowflake::Invalid;
    bool archived = false;
    if (auto ch = instance->getChannel(channelId); ch && ch->isThread()) {
        threadParentId = ch->parentId.hasValue() ? ch->parentId.get() : Snowflake::Invalid;
        archived = ch->isArchived();
    }
    bool isThread = threadParentId.isValid();

    setThreadBrowserTarget(isDm ? Snowflake::Invalid : (isThread ? threadParentId : channelId));

    if (isDm) {
        messageInput->setEnabled(true);
        messageInput->setSendBlocked(false);
        messageInput->setPlaceholder("Message @" + name);
        chatView->setCanPinMessages(true);
        chatView->setCanManageMessages(false);
        slowModeIndicator->setSlowMode(channelId, 0, false);
        setMemberListVisible(false);
        instance->memberList()->clear();
        return;
    }

    Snowflake permChannel = isThread ? threadParentId : channelId;
    Discord::Permission sendPerm = isThread ? Discord::Permission::SEND_MESSAGES_IN_THREADS : Discord::Permission::SEND_MESSAGES;

    bool canSend = instance->permissions()->hasChannelPermission(userId, permChannel, sendPerm);
    bool canPin = instance->permissions()->hasChannelPermission(userId, permChannel, Discord::Permission::PIN_MESSAGES);
    bool canManage = instance->permissions()->hasChannelPermission(userId, permChannel, Discord::Permission::MANAGE_MESSAGES);

    int rateLimit = instance->getChannelRateLimit(channelId);
    bool canBypass = instance->permissions()->hasChannelPermission(userId, permChannel, Discord::Permission::BYPASS_SLOWMODE);
    slowModeIndicator->setSlowMode(channelId, rateLimit, canBypass);

    bool onCooldown = rateLimit > 0 && !canBypass && slowModeIndicator->isOnCooldown(channelId);
    messageInput->setEnabled(canSend && !archived);
    messageInput->setSendBlocked(onCooldown);
    chatView->setCanPinMessages(canPin);
    chatView->setCanManageMessages(canManage);

    if (archived)
        messageInput->setPlaceholder("This thread is archived");
    else if (!canSend)
        messageInput->setPlaceholder("You do not have permission to send messages");
    else if (onCooldown)
        messageInput->setPlaceholder("Slowmode is active");
    else
        messageInput->setPlaceholder((isThread ? "Message " : "Message #") + name);

    if (guildId.isValid()) {
        setMemberListVisible(true);
        instance->memberList()->setActiveChannel(guildId, isThread ? threadParentId : channelId);
    }
}

void MainWindow::switchChatChannel(Core::Snowflake channelId, Core::Snowflake guildId)
{
    if (channelId == chatModel->getActiveChannelId())
        return;

    if (guildId != cachedGuildId) {
        cachedGuildId = guildId;
        userColorCache.clear();
    }

    chatModel->setActiveChannel(channelId, guildId);
    typingTracker->setActiveChannel(channelId);
    messageInput->clearReplyTarget();
}

void MainWindow::switchActiveInstance(Core::ClientInstance *newInstance)
{
    if (currentInstance) {
        currentInstance->forums()->setCurrentForum({});

        auto *msgs = currentInstance->messages();
        disconnect(msgs, nullptr, chatModel, nullptr);
        disconnect(msgs, nullptr, this, nullptr);
        disconnect(currentInstance->discord(), &Discord::Client::typingStart, this, nullptr);
        disconnect(currentInstance->permissions(), nullptr, this, nullptr);
        disconnect(currentInstance, &Core::ClientInstance::membersUpdated, this, nullptr);
        disconnect(memberListView, nullptr, currentInstance->memberList(), nullptr);
        disconnect(currentInstance->forums(), nullptr, this, nullptr);
    }

    currentInstance = newInstance;
    auto *msgs = currentInstance->messages();

    memberListModel->setManager(currentInstance->memberList());

    forumModel->setManager(currentInstance->forums());
    connect(currentInstance->forums(), &Core::ForumManager::loadingChanged, this,
            [this](Core::Snowflake forumId, bool loading) {
                if (forumId == currentForumId)
                    forumBrowser->setLoading(loading);
            });
    connect(memberListView, &MemberListView::visibleRangeChanged,
            currentInstance->memberList(), &Core::MemberListManager::updateSubscriptionRange);

    chatView->setCurrentUserId(currentInstance->accountId());

    typingTracker->clear();
    typingTracker->setUserManager(currentInstance->users());
    typingTracker->setCurrentUserId(currentInstance->accountId());

    connect(msgs, &MessageManager::messagesReceived, chatModel, &ChatModel::handleIncomingMessages);
    connect(msgs, &MessageManager::messageErrored, chatModel, &ChatModel::handleMessageErrored);
    connect(msgs, &MessageManager::messageDeleted, chatModel, &ChatModel::handleMessageDeleted);
    connect(msgs, &MessageManager::attachmentUploadProgress, chatModel, &ChatModel::handleUploadProgress);
    connect(msgs, &MessageManager::messagesReceived, this,
            [this](const MessageRequestResult &result) {
                if (result.success && result.type == Discord::Client::MessageLoadType::History &&
                    result.channelId == chatModel->getActiveChannelId())
                    chatView->onHistoryRequestFinished();
            });

    connect(currentInstance->discord(), &Discord::Client::typingStart, this,
            &MainWindow::onTypingStart);
    connect(currentInstance->discord(), &Discord::Client::messageCreated, this,
            [this](const Discord::Message &msg) {
                typingTracker->removeTyper(msg.channelId, msg.author->id);
            });

    connect(currentInstance->permissions(), &Core::PermissionManager::channelPermissionsChanged,
            this, &MainWindow::onChannelPermissionsChanged);

    connect(currentInstance, &Core::ClientInstance::membersUpdated, this,
            [this](Snowflake guildId, const QList<Snowflake> &userIds) {
                for (const auto &userId : userIds)
                    userColorCache.remove(userId);

                chatModel->refreshUsersInView(userIds);
                forumModel->refreshAuthors();
            });

#ifndef ACHERON_NO_VOICE
    updateVoiceStatusLabel();
#endif
}

void MainWindow::setViewMode(ViewMode mode)
{
    viewMode = mode;

    if (currentInstance)
        currentInstance->forums()->setCurrentForum(
                mode == ViewMode::TextChannel ? Core::Snowflake() : currentForumId);

    switch (mode) {
    case ViewMode::TextChannel:
        forumBrowser->setVisible(false);
        threadPane->setVisible(true);
        threadHeader->setVisible(false);
        break;
    case ViewMode::ForumBrowse:
        forumBrowser->setVisible(true);
        threadPane->setVisible(false);
        threadHeader->setVisible(false);
        break;
    case ViewMode::ForumSplit:
        forumBrowser->setVisible(true);
        threadPane->setVisible(true);
        threadHeader->setVisible(true);
        popOutButton->setText(tr("Open as channel"));
        break;
    case ViewMode::ThreadPopout:
        forumBrowser->setVisible(false);
        threadPane->setVisible(true);
        threadHeader->setVisible(true);
        popOutButton->setText(tr("Back to forum"));
        break;
    }

    updateMemberListVisibility();
}

void MainWindow::setMemberListVisible(bool visible)
{
    memberListWanted = visible;
    updateMemberListVisibility();
}

void MainWindow::updateMemberListVisibility()
{
    memberListView->setVisible(memberListWanted && viewMode == ViewMode::TextChannel);
}

TabEntry MainWindow::makeTabEntry(ChannelNode *node, ChannelNode *accountNode) const
{
    ChannelNode *guildNode = ChannelTreeModel::findGuildNode(node);

    TabEntry entry;
    entry.channelId = node->id;
    entry.guildId = guildNode ? guildNode->id : Snowflake::Invalid;
    entry.accountId = accountNode->id;
    entry.name = node->name;
    entry.isDm = (node->type == ChannelNode::Type::DMChannel);
    entry.isForum = (node->type == ChannelNode::Type::Forum);
    if (guildNode && !guildNode->TEMP_iconHash.isEmpty())
        entry.iconUrl = Discord::Cdn::guildIcon(guildNode->id, guildNode->TEMP_iconHash, 64);
    return entry;
}

void MainWindow::openForumChannel(Core::ClientInstance *instance, Core::Snowflake forumId,
                                  Core::Snowflake guildId)
{
    instance->readState()->setActiveChannel(forumId);

    currentForumId = forumId;
    currentForumGuildId = guildId;
    forumBrowser->setSortMode(static_cast<int>(instance->forums()->sortMode(forumId)));
    forumModel->setForum(forumId, guildId);
    setViewMode(ViewMode::ForumBrowse);
    forumBrowser->setLoading(instance->forums()->isLoading(forumId));

    Snowflake newestPost = instance->readState()->getChannelLastMessageId(forumId);
    if (newestPost.isValid())
        instance->readState()->markChannelAsRead(forumId, newestPost);
}

void MainWindow::openForumPost(Core::Snowflake threadId, Core::Snowflake guildId)
{
    if (!currentInstance)
        return;

    setViewMode(ViewMode::ForumSplit);

    Core::Snowflake userId = currentInstance->accountId();
    bool canSend = currentInstance->permissions()->hasChannelPermission(userId, currentForumId, Discord::Permission::SEND_MESSAGES_IN_THREADS);
    messageInput->setEnabled(canSend);
    messageInput->setSendBlocked(false);
    messageInput->setMaxUploadSize(currentInstance->discord()->getMaxUploadSize(threadId));
    messageInput->setPlaceholder(canSend ? tr("Message this post")
                                         : tr("You do not have permission to send messages"));
    chatView->setCanPinMessages(false);
    chatView->setCanManageMessages(false);

    switchChatChannel(threadId, guildId);
    currentInstance->readState()->setActiveChannel(threadId);
    Snowflake lastMsgId = currentInstance->readState()->getChannelLastMessageId(threadId);
    currentInstance->readState()->markForumPostAsRead(threadId, lastMsgId.isValid() ? lastMsgId : threadId);
    currentInstance->messages()->requestLoadChannel(threadId);
}

void MainWindow::onNewPostRequested()
{
    if (!currentInstance || !currentForumId.isValid())
        return;

    Core::ClientInstance *instance = currentInstance;
    Core::Snowflake forumId = currentForumId;
    Core::Snowflake guildId = currentForumGuildId;

    auto *dialog = new NewPostDialog(instance->forums()->availableTags(forumId), instance->forums()->requiresTag(forumId), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMaxUploadSize(instance->discord()->getMaxUploadSize(forumId));

    connect(dialog, &NewPostDialog::submitted, this, [this, dialog, instance, forumId, guildId]() {
        dialog->setBusy(true);

        QPointer<NewPostDialog> guard(dialog);
        instance->discord()->createForumThread(
                forumId, dialog->title(), dialog->selectedTagIds(), dialog->content(),
                Core::Snowflake::generateNonce().toString(), dialog->attachments(),
                [this, guard, instance, forumId,
                 guildId](const Core::Result<Discord::Client::CreatedForumThread> &res) {
                    if (!res.success()) {
                        qCWarning(LogUI) << "Failed to create forum post:" << res.error;
                        if (guard)
                            guard->showError(tr("Could not post: %1").arg(res.error));
                        return;
                    }

                    if (guard)
                        guard->accept();

                    Core::Snowflake threadId = res.value->thread.id.get();
                    if (res.value->starterMessage)
                        instance->forums()->addStarterMessage(threadId, *res.value->starterMessage);
                    if (currentInstance == instance && currentForumId == forumId)
                        openForumPost(threadId, guildId);
                });
    });

    dialog->open();
}

void MainWindow::setupPermanentConnections(Core::ClientInstance *instance)
{
    if (!instance)
        return;

    if (instancesSignalsConnected.contains(instance->accountId()))
        return;
    instancesSignalsConnected.insert(instance->accountId());

    connect(instance, &Core::ClientInstance::guildCreated, this,
            [this, instance](const Discord::GatewayGuild &guild) {
                channelTreeModel->addGuild(guild, instance->accountId());
                if (channelListMode == ChannelListMode::Tree)
                    channelTree->performDefaultExpansion();
            });

    connect(instance, &Core::ClientInstance::channelCreated, this,
            [this, instance](const Discord::ChannelCreate &event) {
                channelTreeModel->addChannel(event, instance->accountId());
            });

    connect(instance, &Core::ClientInstance::channelUpdated, this,
            [this, instance](const Discord::ChannelUpdate &update) {
                channelTreeModel->updateChannel(update, instance->accountId());

                if (!update.channel.hasValue())
                    return;
                const auto &ch = update.channel.get();
                if (!ch.id.hasValue() || ch.id.get() != chatModel->getActiveChannelId())
                    return;
                if (instance != currentInstance)
                    return;

                int rateLimit = ch.rateLimitPerUser.hasValue() ? ch.rateLimitPerUser.get() : 0;
                Snowflake userId = instance->accountId();
                bool canBypass = instance->permissions()->hasChannelPermission(
                        userId, ch.id.get(), Discord::Permission::BYPASS_SLOWMODE);
                slowModeIndicator->setSlowMode(ch.id.get(), rateLimit, canBypass);
            });

    connect(instance, &Core::ClientInstance::channelDeleted, this,
            [this, instance](const Discord::ChannelDelete &event) {
                channelTreeModel->deleteChannel(event, instance->accountId());
            });

    connect(instance, &Core::ClientInstance::threadCreated, this,
            [this, instance](const Discord::Channel &thread) {
                channelTreeModel->addThread(thread, instance->accountId());
            });
    connect(instance, &Core::ClientInstance::threadUpdated, this,
            [this, instance](const Discord::Channel &thread) {
                channelTreeModel->updateThread(thread, instance->accountId());
            });
    connect(instance, &Core::ClientInstance::threadDeleted, this,
            [this, instance](Core::Snowflake threadId, Core::Snowflake, Core::Snowflake) {
                channelTreeModel->removeThread(threadId, instance->accountId());
            });
    connect(instance, &Core::ClientInstance::threadListSynced, this,
            [this, instance](Core::Snowflake guildId, const QList<Core::Snowflake> &parentIds,
                             const QList<Discord::Channel> &threads) {
                channelTreeModel->syncThreads(guildId, parentIds, threads, instance->accountId());
            });
    connect(instance, &Core::ClientInstance::threadMembershipChanged, this,
            [this, instance](Core::Snowflake threadId) {
                Core::Snowflake acc = instance->accountId();
                if (instance->isThreadJoined(threadId)) {
                    if (channelTreeModel->findChannelTreeNode(threadId, acc))
                        channelTreeModel->promoteTemporaryThread(threadId);
                    else if (auto ch = instance->getChannel(threadId))
                        channelTreeModel->addThread(*ch, acc);
                    channelTreeModel->updateReadState(threadId, acc);
                } else if (chatModel->getActiveChannelId() == threadId) {
                    if (auto ch = instance->getChannel(threadId))
                        channelTreeModel->showTemporaryThread(*ch, acc);
                } else {
                    channelTreeModel->removeThread(threadId, acc);
                }
            });

    // todo: i dont really like the refresh users logic rn
    connect(instance, &Core::ClientInstance::guildRoleCreated, this,
            [this](const Discord::GuildRoleCreate &event) {
                if (event.guildId.hasValue())
                    refreshGuildRoleData(event.guildId.get());
            });

    connect(instance, &Core::ClientInstance::guildRoleUpdated, this,
            [this](const Discord::GuildRoleUpdate &event) {
                if (event.guildId.hasValue())
                    refreshGuildRoleData(event.guildId.get());
            });

    connect(instance, &Core::ClientInstance::guildRoleDeleted, this,
            [this](const Discord::GuildRoleDelete &event) {
                if (event.guildId.hasValue())
                    refreshGuildRoleData(event.guildId.get());
            });

    connect(instance, &Core::ClientInstance::readStateChanged, this,
            [this, instance](Core::Snowflake channelId) {
                channelTreeModel->updateReadState(channelId, instance->accountId());
                refreshTabReadStates();
                forumModel->refreshPost(channelId);
            });

    connect(instance, &Core::ClientInstance::forumBadgeChanged, this,
            [this, instance](Core::Snowflake forumId) {
                channelTreeModel->updateForumBadge(forumId, instance->accountId());
            });

    connect(instance, &Core::ClientInstance::forumJoinedPostsChanged, this,
            [this, instance](Core::Snowflake forumId) {
                channelTreeModel->updateForumThreads(forumId, instance->accountId());
            });

    connect(instance, &Core::ClientInstance::channelLastMessageUpdated, this,
            [this, instance](Core::Snowflake channelId, Core::Snowflake messageId) {
                channelTreeModel->updateChannelLastMessageId(channelId, messageId,
                                                             instance->accountId());
                refreshTabReadStates();
            });

    connect(instance, &Core::ClientInstance::guildSettingsChanged, this,
            [this, instance](Core::Snowflake guildId) {
                channelTreeModel->updateGuildSettings(guildId, instance->accountId());
            });

    connect(instance, &Core::ClientInstance::reconnecting, this,
            [this](int attempt, int maxAttempts) {
                connectionBanner->showReconnecting(attempt, maxAttempts);
            });

    connect(instance, &Core::ClientInstance::stateChanged, this,
            [this](Core::ConnectionState state) {
                if (state == Core::ConnectionState::Connected)
                    connectionBanner->hide();
            });

    connect(instance, &Core::ClientInstance::authenticationFailed, this,
            [this](const Core::AccountInfo &info) {
                connectionBanner->hide();
                const QString accountLabel = info.displayName.isEmpty() ? info.username
                                                                        : info.displayName;
                QWidget *parent = (accountsWindow && accountsWindow->isVisible())
                                          ? static_cast<QWidget *>(accountsWindow)
                                          : static_cast<QWidget *>(this);
                auto *box = new QMessageBox(parent);
                box->setAttribute(Qt::WA_DeleteOnClose);
                box->setIcon(QMessageBox::Critical);
                box->setWindowTitle(tr("Authentication Failed"));
                box->setText(tr("Discord rejected the token for account \"%1\".").arg(accountLabel));
                box->setInformativeText(tr("Discord's gateway rejected your token. The stored token is invalid. Check or update your token and try again."));
                box->setStandardButtons(QMessageBox::Ok);
                box->setWindowModality(Qt::WindowModal);
                box->show();
            });

    connect(instance, &Core::ClientInstance::voiceStateChanged, this,
            [this, instance](Core::Snowflake channelId, Core::Snowflake) {
                channelTree->setAccountVoiceChannel(instance->accountId(), channelId);
#ifndef ACHERON_NO_VOICE
                updateVoiceStatusLabel();
#endif
            });

#ifndef ACHERON_NO_VOICE
    connect(instance->voice(), &Core::AV::VoiceManager::voiceStateChanged,
            this, &MainWindow::updateVoiceStatusLabel);

    connect(instance->voice(), &Core::AV::VoiceManager::channelVoiceMemberChanged,
            this, [this, instance](Core::Snowflake channelId, Core::Snowflake userId, bool joined) {
                int count = instance->voice()->channelVoiceUserCount(channelId);
                channelTreeModel->updateVoiceCount(channelId, count, instance->accountId());
                channelTreeModel->updateVoiceParticipant(channelId, userId, joined,
                                                         instance->accountId());
            });

    connect(instance->voice(), &Core::AV::VoiceManager::participantVoiceStateChanged,
            this, [this, instance](Core::Snowflake channelId, Core::Snowflake userId) {
                channelTreeModel->updateVoiceParticipantState(channelId, userId,
                                                              instance->accountId());
            });
#endif
}

void MainWindow::setupUi()
{
    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);

    channelTree = new ChannelTreeView(this);

#ifndef ACHERON_NO_VOICE
    voiceStatusBar = new VoiceStatusBar(this);
    voiceStatusBar->setImageManager(session->getImageManager());
    connect(voiceStatusBar, &VoiceStatusBar::disconnectRequested, this, [this]() {
        for (const auto &inst : session->getClients()) {
            if (inst && inst->isInVoice()) {
                inst->discord()->sendVoiceStateUpdate(inst->voiceGuildId(), Snowflake::Invalid, false, false);
                break;
            }
        }
    });
#endif

    serverRail = new ServerRailView(this);
    serverRail->setModel(serverRailModel);
    serverRail->setItemDelegate(new ServerRailDelegate(serverRail));
    serverRail->setFixedWidth(ServerRailDelegate::RailWidth);
    serverRail->hide();
    connect(serverRail, &ServerRailView::accountHomeClicked, this, &MainWindow::onRailAccountHomeClicked);
    connect(serverRail, &ServerRailView::guildClicked, this, &MainWindow::onRailGuildClicked);
    connect(serverRail, &ServerRailView::folderToggleClicked, this,
            [this](Snowflake accountId, Snowflake folderId) {
                serverRailModel->toggleFolder(accountId, folderId);
            });
    connect(serverRail, &ServerRailView::markAsReadRequested, this,
            [this](Snowflake accountId, Snowflake id, bool isFolder) {
                markIndexAsRead(accountId, isFolder ? channelTreeModel->folderIndex(accountId, id)
                                                    : channelTreeModel->serverIndex(accountId, id));
            });

    guildHeaderLabel = new QLabel(this);
    guildHeaderLabel->setContentsMargins(12, 8, 12, 8);
    {
        QFont headerFont = guildHeaderLabel->font();
        headerFont.setBold(true);
        guildHeaderLabel->setFont(headerFont);
    }
    guildHeaderLabel->hide();

    auto *rightSideWidget = new QWidget(central);
    auto *rightLayout = new QVBoxLayout(rightSideWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    connectionBanner = new ConnectionBanner(rightSideWidget);
    tabBar = new TabBar(session->getImageManager(), rightSideWidget);

    threadBrowserButton = new QToolButton(rightSideWidget);
    threadBrowserButton->setText(tr("Threads"));
    threadBrowserButton->setIcon(Core::Theme::Icons::icon(Core::Theme::Icons::Name::Spool, Core::Theme::Token::PrimaryText));
    threadBrowserButton->setIconSize(QSize(16, 16));
    threadBrowserButton->setToolTip(tr("Browse threads"));
    threadBrowserButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    threadBrowserButton->setAutoRaise(true);
    threadBrowserButton->setCursor(Qt::PointingHandCursor);
    connect(threadBrowserButton, &QToolButton::clicked, this, &MainWindow::openThreadBrowser);

    channelToolbar = new QWidget(rightSideWidget);
    channelToolbar->setObjectName("channelToolbar");
    channelToolbar->setAttribute(Qt::WA_StyledBackground, true);
    auto *channelToolbarLayout = new QHBoxLayout(channelToolbar);
    channelToolbarLayout->setContentsMargins(8, 3, 8, 3);
    channelToolbarLayout->setSpacing(4);
    channelToolbarLayout->addStretch(1);
    channelToolbarLayout->addWidget(threadBrowserButton, 0);
    channelToolbar->setStyleSheet(
            "#channelToolbar { border-bottom: 1px solid rgba(128, 128, 128, 0.25); }");
    channelToolbar->hide();

    chatView = new ChatView(rightSideWidget);
    chatView->setFont(Core::Theme::Manager::instance().font(Core::Theme::FontRole::Message));
    messageInput = new MessageInput(rightSideWidget);
    typingIndicator = new TypingIndicator(rightSideWidget);
    slowModeIndicator = new SlowModeIndicator(rightSideWidget);

    auto *statusRow = new QWidget(rightSideWidget);
    auto *statusRowLayout = new QHBoxLayout(statusRow);
    statusRowLayout->setContentsMargins(0, 0, 0, 0);
    statusRowLayout->setSpacing(0);
    statusRowLayout->addWidget(typingIndicator, 1);
    statusRowLayout->addWidget(slowModeIndicator, 0);
    statusRow->setFixedHeight(typingIndicator->minimumHeight());

    connect(slowModeIndicator, &SlowModeIndicator::cooldownChanged, this,
            [this](bool onCooldown) {
                if (!currentInstance)
                    return;
                Snowflake channelId = chatModel->getActiveChannelId();
                if (!channelId.isValid())
                    return;
                messageInput->setSendBlocked(onCooldown);
                if (!onCooldown) {
                    QModelIndex current = channelTree->currentIndex();
                    if (current.isValid()) {
                        auto *node = channelTreeModel->nodeFromIndex(
                                channelFilterProxy->mapToSource(current));
                        if (node)
                            messageInput->setPlaceholder("Message #" + node->name);
                    }
                }
            });

    threadHeader = new QWidget(rightSideWidget);
    auto *threadHeaderLayout = new QHBoxLayout(threadHeader);
    threadHeaderLayout->setContentsMargins(8, 4, 8, 4);
    popOutButton = new QPushButton(tr("Open as channel"), threadHeader);
    closeThreadButton = new QPushButton(tr("Close"), threadHeader);
    threadHeaderLayout->addStretch(1);
    threadHeaderLayout->addWidget(popOutButton, 0);
    threadHeaderLayout->addWidget(closeThreadButton, 0);
    threadHeader->setVisible(false);

    connect(popOutButton, &QPushButton::clicked, this, [this]() {
        setViewMode(viewMode == ViewMode::ThreadPopout ? ViewMode::ForumSplit
                                                       : ViewMode::ThreadPopout);
    });
    connect(closeThreadButton, &QPushButton::clicked, this,
            [this]() { setViewMode(ViewMode::ForumBrowse); });

    threadPane = new QWidget(rightSideWidget);
    auto *threadPaneLayout = new QVBoxLayout(threadPane);
    threadPaneLayout->setContentsMargins(0, 0, 0, 0);
    threadPaneLayout->setSpacing(0);
    threadPaneLayout->addWidget(threadHeader, 0);
    threadPaneLayout->addWidget(chatView, 1);
    threadPaneLayout->addWidget(statusRow, 0);
    threadPaneLayout->addWidget(messageInput, 0);

    forumModel = new ForumPostModel(session->getImageManager(), this);
    forumModel->setDisplayNameResolver([this](Snowflake userId, Snowflake guildId) -> QString {
        if (!currentInstance || !guildId.isValid())
            return QString();
        auto member = currentInstance->users()->getMember(guildId, userId);
        if (member && member->nick.hasValue())
            return member->nick.get();
        return QString();
    });
    forumModel->setRoleColorResolver([this](Snowflake userId, Snowflake guildId) { return resolveRoleColor(userId, guildId); });
    forumBrowser = new ForumBrowser(rightSideWidget);
    forumBrowser->setModel(forumModel);
    forumBrowser->setVisible(false);

    connect(forumBrowser, &ForumBrowser::postActivated, this, &MainWindow::openForumPost);
    connect(forumBrowser, &ForumBrowser::newPostRequested, this, &MainWindow::onNewPostRequested);
    connect(forumBrowser, &ForumBrowser::sortModeChanged, this, [this](int mode) {
        if (currentInstance && currentForumId.isValid())
            currentInstance->forums()->setSortMode(currentForumId, static_cast<Core::ForumSortMode>(mode));
    });

    centerSplitter = new QSplitter(Qt::Horizontal, rightSideWidget);
    centerSplitter->addWidget(forumBrowser);
    centerSplitter->addWidget(threadPane);
    centerSplitter->setStretchFactor(0, 1);
    centerSplitter->setStretchFactor(1, 1);
    centerSplitter->setSizes({ 350, 550 });

    rightLayout->addWidget(connectionBanner, 0);
    rightLayout->addWidget(tabBar, 0);
    rightLayout->addWidget(channelToolbar, 0);
    rightLayout->addWidget(centerSplitter, 1);

    memberListView = new MemberListView(central);
    memberListView->setModel(memberListModel);
    memberListView->setItemDelegate(new MemberListDelegate(memberListView));

    connect(&Core::Theme::Manager::instance(), &Core::Theme::Manager::themeChanged, this, [this]() {
        chatModel->invalidateDocCache();
        chatView->viewport()->update();
        channelTree->viewport()->update();
        memberListView->viewport()->update();
    });

    connect(&Core::Theme::Manager::instance(), &Core::Theme::Manager::metricsChanged, this, [this]() {
        chatView->setFont(Core::Theme::Manager::instance().font(Core::Theme::FontRole::Message));
        chatModel->invalidateLayout();
        chatView->doItemsLayout();
        channelTree->viewport()->update();
        memberListView->viewport()->update();
    });

    connect(memberListView, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
                QModelIndex idx = memberListView->indexAt(pos);
                if (!idx.isValid())
                    return;
                if (idx.data(MemberListModel::ItemTypeRole).toInt() != static_cast<int>(Core::MemberListItem::Type::Member))
                    return;
                Snowflake userId = idx.data(MemberListModel::UserIdRole).toULongLong();
                Snowflake guildId = currentInstance
                                            ? currentInstance->memberList()->currentGuildId()
                                            : Snowflake::Invalid;
                showUserContextMenu(userId, guildId,
                                    memberListView->viewport()->mapToGlobal(pos));
            });

    leftSideWidget = buildLeftSide();

    mainSplitter = new QSplitter(this);
    mainSplitter->addWidget(leftSideWidget);
    mainSplitter->addWidget(rightSideWidget);
    mainSplitter->addWidget(memberListView);

    mainSplitter->setCollapsible(0, false);
    mainSplitter->setCollapsible(2, false);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setStretchFactor(2, 0);
    memberListView->setMinimumWidth(140);
    memberListView->setMaximumWidth(400);

    memberListView->hide();

    channelTree->setModel(channelFilterProxy);
    channelTree->setHeaderHidden(true);
    channelTree->setIndentation(0);
    channelTree->setItemDelegate(new ChannelDelegate(channelFilterProxy, channelTree));
    channelTree->setIconSize(QSize(24, 24));
    channelTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    channelTree->setFrameShape(QFrame::NoFrame);
    channelTree->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    channelTree->setExpandsOnDoubleClick(false);

    connect(channelTree, &ChannelTreeView::markAsReadRequested, this,
            [this](const QModelIndex &proxyIndex) {
                QModelIndex sourceIndex = channelFilterProxy->mapToSource(proxyIndex);
                auto *node = channelTreeModel->nodeFromIndex(sourceIndex);
                if (!node)
                    return;

                ChannelNode *accountNode = channelTreeModel->getAccountNodeFor(node);
                if (!accountNode)
                    return;

                markIndexAsRead(accountNode->id, sourceIndex);
            });

    chatView->setModel(chatModel);
    chatView->setItemDelegate(new ChatDelegate(session->getImageManager(), chatView));
    chatView->setIconSize(QSize(24, 24));
    chatView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chatView->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    chatView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    chatView->setUniformItemSizes(false);
    chatView->setWordWrap(true);
    chatView->setResizeMode(QListView::Adjust);

    connect(messageInput, &MessageInput::sendMessage, this, [this](const QString &text, const QList<Core::PendingAttachment> &attachments) {
        if (!currentInstance) {
            qCWarning(LogCore) << "Cannot send message: no active instance";
            return;
        }

        Snowflake channelId = chatModel->getActiveChannelId();
        if (!channelId.isValid()) {
            qCWarning(LogCore) << "Cannot send message: no active channel";
            return;
        }

        if (messageInput->isSendBlocked()) {
            qCDebug(LogCore) << "Cannot send message: slowmode cooldown active";
            return;
        }

        Snowflake replyTo = messageInput->replyTargetMessageId();
        currentInstance->messages()->sendMessage(channelId, text, replyTo, attachments);

        int rateLimit = currentInstance->getChannelRateLimit(channelId);
        Snowflake userId = currentInstance->accountId();
        bool canBypass = currentInstance->permissions()->hasChannelPermission(
                userId, channelId, Discord::Permission::BYPASS_SLOWMODE);
        if (rateLimit > 0 && !canBypass) {
            slowModeIndicator->startCooldown(channelId, rateLimit);
            messageInput->setSendBlocked(true);
            messageInput->setPlaceholder("Slowmode is active");
        }
    });

    connect(chatView, &ChatView::historyRequested, this, [this]() {
        Snowflake oldestId = chatModel->getOldestMessageId();

        if (currentInstance && oldestId.isValid())
            currentInstance->messages()->requestLoadHistory(chatModel->getActiveChannelId(),
                                                            oldestId);
    });

    connect(chatView, &ChatView::filesDropped, this, [this](const QList<QUrl> &urls) {
        messageInput->queueAttachments(urls);
    });

    connect(chatView, &ChatView::cancelUploadRequested, this,
            [this](Snowflake channelId, Snowflake messageId) {
                if (currentInstance)
                    currentInstance->messages()->cancelSend(channelId, QString::number(messageId));
            });

    connect(chatView, &ChatView::deleteMessageRequested, this,
            [this](Snowflake channelId, Snowflake messageId) {
                if (!currentInstance)
                    return;

                if (QApplication::keyboardModifiers() & Qt::ShiftModifier) {
                    currentInstance->discord()->deleteMessage(channelId, messageId);
                    return;
                }

                ConfirmPopup dialog(tr("Delete Message"),
                                    tr("Are you sure you want to delete this message?"),
                                    tr("Delete"), this);
                if (dialog.exec() == QDialog::Accepted)
                    currentInstance->discord()->deleteMessage(channelId, messageId);
            });

    connect(chatView, &ChatView::pinMessageRequested, this,
            [this](Snowflake channelId, Snowflake messageId) {
                if (currentInstance)
                    currentInstance->discord()->pinMessage(channelId, messageId);
            });

    connect(chatView, &ChatView::editMessageRequested, this,
            [this](Snowflake channelId, Snowflake messageId, const QString &content) {
                if (currentInstance)
                    currentInstance->discord()->editMessage(channelId, messageId, content);
            });

    connect(chatView, &ChatView::replyToMessageRequested, this,
            [this](Snowflake channelId, Snowflake messageId) {
                // Find the message in the model to get author name and content
                for (int row = 0; row < chatModel->rowCount(); ++row) {
                    QModelIndex idx = chatModel->index(row, 0);
                    Snowflake msgId = idx.data(ChatModel::MessageIdRole).toULongLong();
                    if (msgId == messageId) {
                        QString authorName = idx.data(ChatModel::UsernameRole).toString();
                        QString content = idx.data(ChatModel::ContentRole).toString();
                        messageInput->setReplyTarget(messageId, authorName, content);
                        return;
                    }
                }
                // Fallback if message not found in model
                messageInput->setReplyTarget(messageId, tr("Unknown"), QString());
            });

    connect(chatView, &ChatView::addReactionRequested, this,
            [this](Snowflake channelId, Snowflake messageId) {
                qCInfo(LogCore) << "Add reaction requested for message" << messageId
                                << "- UI not yet implemented";
            });

    connect(chatView, &ChatView::toggleReactionClicked, this,
            [this](Snowflake channelId, Snowflake messageId, const QString &emoji, bool currentlyReacted, bool isBurst) {
                if (!currentInstance)
                    return;
                if (currentlyReacted)
                    currentInstance->discord()->removeReaction(channelId, messageId, emoji, isBurst);
                else
                    currentInstance->discord()->addReaction(channelId, messageId, emoji, isBurst);
            });

    connect(chatView, &ChatView::userContextMenuRequested, this,
            [this](Snowflake userId, QPoint globalPos) {
                showUserContextMenu(userId, cachedGuildId, globalPos);
            });

    connect(chatView, &ChatView::channelMentionClicked, this, &MainWindow::navigateToChannel);

    connect(channelTree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MainWindow::onChannelSelectionChanged);

    connect(tabBar, &TabBar::tabChanged, this, &MainWindow::switchToTabEntry);

    connect(channelTree, &ChannelTreeView::openInNewTabRequested, this,
            [this](const QModelIndex &proxyIndex) {
                QModelIndex sourceIndex = channelFilterProxy->mapToSource(proxyIndex);
                auto *node = channelTreeModel->nodeFromIndex(sourceIndex);
                if (!node)
                    return;

                ChannelNode *accountNode = channelTreeModel->getAccountNodeFor(node);
                if (!accountNode)
                    return;

                tabBar->openNewTab(makeTabEntry(node, accountNode));
            });

    auto threadMembership = [this](const QModelIndex &proxyIndex, bool join) {
        QModelIndex sourceIndex = channelFilterProxy->mapToSource(proxyIndex);
        auto *node = channelTreeModel->nodeFromIndex(sourceIndex);
        if (!node || node->type != ChannelNode::Type::Thread)
            return;
        ChannelNode *accountNode = channelTreeModel->getAccountNodeFor(node);
        if (!accountNode)
            return;
        auto *instance = session->client(accountNode->id);
        if (!instance)
            return;
        if (join)
            instance->discord()->joinThread(node->id);
        else
            instance->discord()->leaveThread(node->id);
    };
    connect(channelTree, &ChannelTreeView::joinThreadRequested, this, [threadMembership](const QModelIndex &proxyIndex) { threadMembership(proxyIndex, true); });
    connect(channelTree, &ChannelTreeView::leaveThreadRequested, this, [threadMembership](const QModelIndex &proxyIndex) { threadMembership(proxyIndex, false); });

#ifndef ACHERON_NO_VOICE
    connect(channelTree, &ChannelTreeView::joinVoiceChannelRequested, this,
            [this](const QModelIndex &proxyIndex) {
                QModelIndex sourceIndex = channelFilterProxy->mapToSource(proxyIndex);
                auto *node = channelTreeModel->nodeFromIndex(sourceIndex);
                if (!node)
                    return;

                bool isDM = (node->type == ChannelNode::Type::DMChannel);
                bool isVoice = (node->type == ChannelNode::Type::VoiceChannel);
                if (!isDM && !isVoice)
                    return;

                ChannelNode *accountNode = channelTreeModel->getAccountNodeFor(node);
                if (!accountNode)
                    return;

                ClientInstance *instance = session->client(accountNode->id);
                if (!instance)
                    return;

                if (isDM) {
                    qCInfo(LogVoice) << "Joining DM call" << node->name << node->id;
                    instance->discord()->sendVoiceStateUpdate(Snowflake::Invalid, node->id, false, false);
                } else {
                    ChannelNode *guildNode = ChannelTreeModel::findGuildNode(node);
                    if (!guildNode)
                        return;

                    qCInfo(LogVoice) << "Joining voice channel" << node->name << node->id
                                     << "in guild" << guildNode->id;
                    instance->discord()->sendVoiceStateUpdate(guildNode->id, node->id, false, false);
                }
            });

    connect(channelTree, &ChannelTreeView::disconnectVoiceRequested, this,
            [this](const QModelIndex &proxyIndex) {
                QModelIndex sourceIndex = channelFilterProxy->mapToSource(proxyIndex);
                auto *node = channelTreeModel->nodeFromIndex(sourceIndex);
                if (!node)
                    return;

                ChannelNode *accountNode = channelTreeModel->getAccountNodeFor(node);
                if (!accountNode)
                    return;

                ClientInstance *instance = session->client(accountNode->id);
                if (!instance || !instance->isInVoice())
                    return;

                qCInfo(LogVoice) << "Disconnecting from voice";
                instance->discord()->sendVoiceStateUpdate(instance->voiceGuildId(), Snowflake::Invalid, false, false);
            });
#endif

    layout->addWidget(mainSplitter);
    layout->setContentsMargins(0, 0, 4, 0);
    setCentralWidget(central);
}

QWidget *MainWindow::buildLeftSide()
{
    auto *container = new QWidget;

    if (channelListMode == ChannelListMode::Classic) {
        auto *h = new QHBoxLayout(container);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);

        serverRail->show();
        h->addWidget(serverRail, 0);

        auto *secondary = new QWidget(container);
        auto *v = new QVBoxLayout(secondary);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        v->addWidget(guildHeaderLabel, 0);
        guildHeaderLabel->setVisible(!guildHeaderLabel->text().isEmpty());
        channelTree->show();
        v->addWidget(channelTree, 1);
#ifndef ACHERON_NO_VOICE
        if (voiceStatusBar)
            v->addWidget(voiceStatusBar, 0);
#endif
        h->addWidget(secondary, 1);
    } else {
        auto *v = new QVBoxLayout(container);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        channelTree->show();
        v->addWidget(channelTree, 1);
#ifndef ACHERON_NO_VOICE
        if (voiceStatusBar)
            v->addWidget(voiceStatusBar, 0);
#endif
    }

    container->setMinimumWidth(200);
    return container;
}

void MainWindow::setChannelListMode(ChannelListMode mode)
{
    if (channelListMode == mode)
        return;
    channelListMode = mode;
    QSettings().setValue("ui/channelListMode", mode == ChannelListMode::Classic ? "classic" : "tree");

    QList<int> splitterSizes = mainSplitter ? mainSplitter->sizes() : QList<int>();
#ifndef ACHERON_NO_VOICE
    bool voiceVisible = voiceStatusBar && voiceStatusBar->isVisible();
#endif

    channelTree->setParent(this);
    channelTree->hide();
    serverRail->setParent(this);
    serverRail->hide();
    guildHeaderLabel->setParent(this);
    guildHeaderLabel->hide();
#ifndef ACHERON_NO_VOICE
    if (voiceStatusBar) {
        voiceStatusBar->setParent(this);
        voiceStatusBar->hide();
    }
#endif

    QWidget *newLeft = buildLeftSide();
    QWidget *old = mainSplitter->replaceWidget(0, newLeft);
    leftSideWidget = newLeft;
    if (old)
        old->deleteLater();

    mainSplitter->setCollapsible(0, false);
    mainSplitter->setStretchFactor(0, 0);
    if (splitterSizes.size() == mainSplitter->count()) {
        const int delta = (mode == ChannelListMode::Classic) ? ServerRailDelegate::RailWidth : -ServerRailDelegate::RailWidth;
        splitterSizes[0] = qMax(0, splitterSizes[0] + delta);
        if (splitterSizes.size() > 1)
            splitterSizes[1] = qMax(0, splitterSizes[1] - delta);
        mainSplitter->setSizes(splitterSizes);
    }

#ifndef ACHERON_NO_VOICE
    if (voiceStatusBar)
        voiceStatusBar->setVisible(voiceVisible);
#endif

    if (mode == ChannelListMode::Tree) {
        channelTree->setRootIndex({});
        channelTree->performDefaultExpansion();
        applyTreeState();
    } else {
        serverRailModel->rebuild();
        railHasSelection = false;
        selectInitialRailItem();
    }
}

void MainWindow::onRailGuildSelected(Snowflake accountId, Snowflake guildId)
{
    QModelIndex src = channelTreeModel->serverIndex(accountId, guildId);
    if (!src.isValid())
        return;
    QModelIndex proxy = channelFilterProxy->mapFromSource(src);
    if (!proxy.isValid())
        return;

    channelTree->setRootIndex(proxy);
    channelTree->performDefaultExpansion();

    serverRailModel->setSelected(ServerRailModel::Kind::Server, accountId, guildId);
    QString guildName = src.data(Qt::DisplayRole).toString();
    guildHeaderLabel->setText(guildName);
    guildHeaderLabel->setVisible(!guildName.isEmpty());
    railHasSelection = true;
    railSelectedIsHome = false;
    railSelectedAccountId = accountId;
    railSelectedGuildId = guildId;

    QSettings s;
    s.setValue("ui/rail/lastAccountId", static_cast<qulonglong>(accountId));
    s.setValue("ui/rail/lastGuildId", static_cast<qulonglong>(guildId));
    s.setValue("ui/rail/lastIsHome", false);
}

void MainWindow::onRailAccountHomeSelected(Snowflake accountId)
{
    QModelIndex src = channelTreeModel->dmHeaderIndex(accountId);
    if (!src.isValid())
        return;
    QModelIndex proxy = channelFilterProxy->mapFromSource(src);
    if (!proxy.isValid())
        return;

    channelTree->setRootIndex(proxy);

    serverRailModel->setSelected(ServerRailModel::Kind::AccountHome, accountId, accountId);
    guildHeaderLabel->setText(tr("Direct Messages"));
    guildHeaderLabel->show();
    railHasSelection = true;
    railSelectedIsHome = true;
    railSelectedAccountId = accountId;
    railSelectedGuildId = Snowflake();

    QSettings s;
    s.setValue("ui/rail/lastAccountId", static_cast<qulonglong>(accountId));
    s.setValue("ui/rail/lastIsHome", true);
}

void MainWindow::onRailAccountHomeClicked(Snowflake accountId)
{
    onRailAccountHomeSelected(accountId);

    auto it = lastViewedChannel.constFind(guildChannelKey(accountId, Snowflake::Invalid));
    if (it == lastViewedChannel.cend())
        return;

    ChannelNode *node = channelTreeModel->findChannelTreeNode(it.value(), accountId);
    if (!node || node->type != ChannelNode::Type::DMChannel)
        return;

    channelFilterProxy->setSelectedChannel(node->id, accountId);
    QModelIndex proxy = channelFilterProxy->mapFromSource(channelTreeModel->indexForNode(node));
    if (proxy.isValid())
        channelTree->setCurrentIndex(proxy);
}

void MainWindow::selectInitialRailItem()
{
    if (currentInstance) {
        Snowflake channelId = chatModel->getActiveChannelId();
        if (channelId.isValid()) {
            Snowflake accountId = currentInstance->accountId();
            if (ChannelNode *node = channelTreeModel->findChannelTreeNode(channelId, accountId)) {
                if (node->type == ChannelNode::Type::DMChannel) {
                    onRailAccountHomeSelected(accountId);
                    return;
                }
                ChannelNode *guild = ChannelTreeModel::findGuildNode(node);
                if (guild) {
                    onRailGuildSelected(accountId, guild->id);
                    return;
                }
            }
        }
    }

    if (hasSavedRailSelection) {
        if (!savedRailIsHome &&
            channelTreeModel->serverIndex(savedRailAccountId, savedRailGuildId).isValid()) {
            onRailGuildSelected(savedRailAccountId, savedRailGuildId);
            return;
        }
        if (channelTreeModel->dmHeaderIndex(savedRailAccountId).isValid()) {
            onRailAccountHomeSelected(savedRailAccountId);
            return;
        }
    }

    for (int i = 0; i < channelTreeModel->rowCount({}); ++i) {
        QModelIndex accIdx = channelTreeModel->index(i, 0, {});
        if (static_cast<ChannelNode::Type>(accIdx.data(ChannelTreeModel::TypeRole).toInt()) == ChannelNode::Type::Account) {
            onRailAccountHomeSelected(Snowflake(accIdx.data(ChannelTreeModel::IdRole).toULongLong()));
            return;
        }
    }

    guildHeaderLabel->clear();
    guildHeaderLabel->hide();
}

void MainWindow::applyPendingRailSelection(Snowflake accountId)
{
    if (channelListMode != ChannelListMode::Classic)
        return;

    if (railHasSelection) {
        hasSavedRailSelection = false;
        return;
    }

    if (hasSavedRailSelection) {
        if (savedRailAccountId != accountId)
            return;
        hasSavedRailSelection = false;
        if (!savedRailIsHome &&
            channelTreeModel->serverIndex(accountId, savedRailGuildId).isValid())
            onRailGuildSelected(accountId, savedRailGuildId);
        else
            onRailAccountHomeSelected(accountId);
        return;
    }

    onRailAccountHomeSelected(accountId);
}

void MainWindow::onRailGuildClicked(Snowflake accountId, Snowflake guildId)
{
    onRailGuildSelected(accountId, guildId);

    Snowflake channelId = resolveRailChannel(accountId, guildId);
    if (!channelId.isValid())
        return;

    ChannelNode *node = channelTreeModel->findChannelTreeNode(channelId, accountId);
    if (!node)
        return;

    channelFilterProxy->setSelectedChannel(channelId, accountId);
    QModelIndex proxy = channelFilterProxy->mapFromSource(channelTreeModel->indexForNode(node));
    if (proxy.isValid())
        channelTree->setCurrentIndex(proxy);
}

Core::Snowflake MainWindow::resolveRailChannel(Snowflake accountId, Snowflake guildId)
{
    auto it = lastViewedChannel.constFind(guildChannelKey(accountId, guildId));
    if (it != lastViewedChannel.cend() && channelReadable(accountId, guildId, it.value()))
        return it.value();

    QModelIndex guildSrc = channelTreeModel->serverIndex(accountId, guildId);
    ChannelNode *guildNode = guildSrc.isValid() ? channelTreeModel->nodeFromIndex(guildSrc) : nullptr;
    if (guildNode && channelReadable(accountId, guildId, guildNode->rulesChannelId))
        return guildNode->rulesChannelId;

    return firstReadableChannel(accountId, guildId);
}

bool MainWindow::channelReadable(Snowflake accountId, Snowflake guildId, Snowflake channelId)
{
    if (!channelId.isValid())
        return false;
    ChannelNode *node = channelTreeModel->findChannelTreeNode(channelId, accountId);
    if (!node || node->type != ChannelNode::Type::Channel)
        return false;
    ChannelNode *guild = ChannelTreeModel::findGuildNode(node);
    if (!guild || guild->id != guildId)
        return false;
    ClientInstance *instance = session->client(accountId);
    if (instance && instance->permissions())
        return instance->permissions()->hasChannelPermission(accountId, channelId, Discord::Permission::VIEW_CHANNEL);
    return true;
}

Core::Snowflake MainWindow::firstReadableChannel(Snowflake accountId, Snowflake guildId)
{
    QModelIndex guildSrc = channelTreeModel->serverIndex(accountId, guildId);
    if (!guildSrc.isValid())
        return {};
    ChannelNode *guildNode = channelTreeModel->nodeFromIndex(guildSrc);
    if (!guildNode)
        return {};

    ClientInstance *instance = session->client(accountId);
    auto *perms = instance ? instance->permissions() : nullptr;
    auto readable = [&](Snowflake ch) {
        return !perms || perms->hasChannelPermission(accountId, ch, Discord::Permission::VIEW_CHANNEL);
    };

    for (const auto &child : guildNode->children) {
        if (child->type == ChannelNode::Type::Category)
            break;
        if (child->type == ChannelNode::Type::Channel && readable(child->id))
            return child->id;
    }

    for (const auto &child : guildNode->children) {
        if (child->type != ChannelNode::Type::Category)
            continue;
        for (const auto &cc : child->children)
            if (cc->type == ChannelNode::Type::Channel && readable(cc->id))
                return cc->id;
    }
    return {};
}

void MainWindow::markIndexAsRead(Snowflake accountId, const QModelIndex &sourceIndex)
{
    ClientInstance *instance = session->client(accountId);
    if (!instance || !sourceIndex.isValid())
        return;

    const auto pairs = channelTreeModel->getMarkableChannels(sourceIndex);
    if (pairs.size() == 1)
        instance->readState()->markChannelAsRead(pairs.first().first, pairs.first().second);
    else if (!pairs.isEmpty())
        instance->readState()->markChannelsAsRead(pairs);
}

void MainWindow::recordLastViewedChannel(Snowflake accountId, Snowflake guildId, Snowflake channelId)
{
    lastViewedChannel.insert(guildChannelKey(accountId, guildId), channelId);
}

#ifndef ACHERON_NO_VOICE
void MainWindow::updateVoiceStatusLabel()
{
    using VState = Discord::AV::VoiceClient::State;

    // Find any account that is in voice or has an active voice client
    Core::ClientInstance *voiceInstance = nullptr;
    for (const auto &inst : session->getClients()) {
        if (inst && (inst->isInVoice() || inst->voice()->clientState() != VState::Disconnected)) {
            voiceInstance = inst;
            break;
        }
    }

    Core::AV::VoiceManager *vm = voiceInstance ? voiceInstance->voice() : nullptr;
    voiceStatusBar->setVoiceManager(vm);

    if (voiceInstance) {
        QPointer<Core::UserManager> um = voiceInstance->users();
        Core::Snowflake vGuildId = voiceInstance->voiceGuildId();
        voiceStatusBar->setNameResolver([um, vGuildId](Core::Snowflake userId) -> QString {
            if (!um)
                return QString::number(userId);
            return um->getDisplayName(userId, vGuildId);
        });
        voiceStatusBar->setAvatarResolver([um](Core::Snowflake userId) -> QUrl {
            if (!um)
                return {};
            auto user = um->getUser(userId);
            if (!user)
                return {};
            return Discord::Cdn::userAvatar(userId, user->avatar.get(), 32);
        });
    } else {
        voiceStatusBar->setNameResolver(nullptr);
        voiceStatusBar->setAvatarResolver(nullptr);
    }

    QString channelName;
    if (voiceInstance) {
        Core::Snowflake vcId = voiceInstance->voiceChannelId();
        if (vcId.isValid()) {
            ChannelNode *node = channelTreeModel->findChannelTreeNode(vcId);
            if (node) {
                bool isDm = (node->type == ChannelNode::Type::DMChannel);
                channelName = isDm ? node->name : ("#" + node->name);
            } else {
                channelName = QString::number(vcId);
            }
        }
    }
    voiceStatusBar->setChannelName(channelName);
}
#endif

void MainWindow::switchToTabEntry(const TabEntry &entry)
{
    if (!entry.channelId.isValid())
        return;

    activateChannel(entry);
}

void MainWindow::activateChannel(const TabEntry &entry)
{
    channelTreeModel->clearTemporaryThread(entry.channelId);

    // update the proxy selected channel so the delegate highlights correctly,
    // and clear the trees own selection so no stale highlight remains
    channelFilterProxy->setSelectedChannel(entry.channelId, entry.accountId);
    {
        QSignalBlocker blocker(channelTree->selectionModel());
        channelTree->selectionModel()->clearSelection();
        channelTree->selectionModel()->clearCurrentIndex();
    }
    channelTree->viewport()->update();

    if (channelListMode == ChannelListMode::Classic) {
        if (entry.isDm) {
            if (!(railSelectedIsHome && railSelectedAccountId == entry.accountId))
                onRailAccountHomeSelected(entry.accountId);
        } else if (entry.guildId.isValid()) {
            if (railSelectedIsHome || railSelectedAccountId != entry.accountId ||
                railSelectedGuildId != entry.guildId)
                onRailGuildSelected(entry.accountId, entry.guildId);
        }
    }

    ClientInstance *instance = session->client(entry.accountId);
    if (!instance) {
        messageInput->setEnabled(false);
        return;
    }

    if (instance != currentInstance)
        switchActiveInstance(instance);

    if (entry.isForum) {
        setThreadBrowserTarget(Snowflake::Invalid);
        openForumChannel(instance, entry.channelId, entry.guildId);
        if (entry.guildId.isValid())
            recordLastViewedChannel(entry.accountId, entry.guildId, entry.channelId);
        refreshTabReadStates();
        return;
    }

    instance->readState()->setActiveChannel(entry.channelId);
    setViewMode(ViewMode::TextChannel);

    if (!entry.isDm && entry.guildId.isValid())
        recordLastViewedChannel(entry.accountId, entry.guildId, entry.channelId);
    else if (entry.isDm)
        recordLastViewedChannel(entry.accountId, Snowflake::Invalid, entry.channelId);

    applyChannelChrome(instance, entry.channelId, entry.name, entry.isDm, entry.guildId);
    switchChatChannel(entry.channelId, entry.guildId);
    instance->messages()->requestLoadChannel(entry.channelId);

    Snowflake lastMsgId = instance->readState()->getChannelLastMessageId(entry.channelId);
    if (lastMsgId.isValid())
        instance->readState()->markChannelAsRead(entry.channelId, lastMsgId);

    refreshTabReadStates();
}

void MainWindow::refreshTabReadStates()
{
    for (int i = 0; i < tabBar->tabCount(); ++i) {
        const TabEntry &entry = tabBar->tabEntry(i);
        if (!entry.channelId.isValid())
            continue;

        ClientInstance *inst = session->client(entry.accountId);
        if (!inst)
            continue;

        auto state = inst->readState()->computeChannelReadState(
                entry.channelId, entry.guildId, Snowflake::Invalid, entry.isDm);
        if (entry.isForum) {
            auto posts = inst->forums()->joinedPostsContribution(entry.channelId);
            state.isUnread = state.isUnread || posts.unread;
            state.mentionCount += posts.mentions;
        }
        tabBar->updateChannelReadState(entry.channelId, state.isUnread, state.mentionCount);
    }
}

namespace {

bool isNativeExpandable(ChannelNode::Type type)
{
    return type == ChannelNode::Type::Account ||
           type == ChannelNode::Type::DMHeader ||
           type == ChannelNode::Type::Folder ||
           type == ChannelNode::Type::Server;
}

// <accountId>:<type>:<nodeId>
QStringList mergeTreeKeys(const QStringList &previous, const QStringList &captured,
                          const QSet<QString> &presentAccounts)
{
    QStringList result;
    for (const QString &key : previous) {
        if (!presentAccounts.contains(key.section(':', 0, 0)))
            result.append(key);
    }
    result.append(captured);
    return result;
}

} // namespace

void MainWindow::forEachSourceNode(const std::function<void(const QModelIndex &, ChannelNode *)> &fn) const
{
    std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
        int rows = channelTreeModel->rowCount(parent);
        for (int i = 0; i < rows; ++i) {
            QModelIndex idx = channelTreeModel->index(i, 0, parent);
            if (ChannelNode *node = channelTreeModel->nodeFromIndex(idx))
                fn(idx, node);
            walk(idx);
        }
    };
    walk({});
}

QString MainWindow::treeNodeKey(const ChannelNode *node) const
{
    const ChannelNode *acct = node;
    while (acct && acct->type != ChannelNode::Type::Account)
        acct = acct->parent;

    quint64 acctId = acct ? static_cast<quint64>(acct->id) : 0;
    return QStringLiteral("%1:%2:%3")
            .arg(acctId)
            .arg(static_cast<int>(node->type))
            .arg(static_cast<quint64>(node->id));
}

void MainWindow::captureTreeState(QStringList &expanded,
                                  QStringList &collapsed,
                                  QStringList &collapsedCategories,
                                  QSet<QString> &presentAccounts) const
{
    forEachSourceNode([&](const QModelIndex &sourceIndex, ChannelNode *node) {
        if (node->type == ChannelNode::Type::Account)
            presentAccounts.insert(QString::number(static_cast<quint64>(node->id)));

        if (node->type == ChannelNode::Type::Category) {
            if (node->collapsed)
                collapsedCategories.append(treeNodeKey(node));
            return;
        }

        if (!isNativeExpandable(node->type))
            return;

        QModelIndex proxyIndex = channelFilterProxy->mapFromSource(sourceIndex);
        if (!proxyIndex.isValid())
            return;

        if (channelTree->isExpanded(proxyIndex))
            expanded.append(treeNodeKey(node));
        else
            collapsed.append(treeNodeKey(node));
    });
}

void MainWindow::applyTreeState()
{
    if (!hasSavedTreeState)
        return;

    // first restore the pseudo-collapsed stuff for categories
    forEachSourceNode([this](const QModelIndex &sourceIndex, ChannelNode *node) {
        if (node->type == ChannelNode::Type::Category)
            channelTreeModel->setCollapsed(sourceIndex, savedCollapsedCategories.contains(treeNodeKey(node)));
    });
    channelFilterProxy->invalidateFilter();

    // the rest
    forEachSourceNode([this](const QModelIndex &sourceIndex, ChannelNode *node) {
        if (!isNativeExpandable(node->type))
            return;

        QString key = treeNodeKey(node);
        QModelIndex proxyIndex = channelFilterProxy->mapFromSource(sourceIndex);
        if (!proxyIndex.isValid())
            return;

        if (savedExpandedNodes.contains(key))
            channelTree->expand(proxyIndex);
        else if (savedCollapsedNodes.contains(key))
            channelTree->collapse(proxyIndex);
    });
}

void MainWindow::maybeActivatePendingChannel(Core::Snowflake accountId)
{
    if (!pendingActiveEntry.has_value() || pendingActiveEntry->accountId != accountId)
        return;

    TabEntry entry = *pendingActiveEntry;
    pendingActiveEntry.reset();
    activateChannel(entry);
}

void MainWindow::saveWindowState()
{
    QSettings settings;

    settings.setValue("layout/geometry", saveGeometry());
    if (mainSplitter)
        settings.setValue("layout/splitter", mainSplitter->saveState());

    const QList<TabEntry> all = tabBar->tabEntries();
    int activeIndex = tabBar->activeTabIndex();
    QList<TabEntry> valid;
    int newActive = 0;
    for (int i = 0; i < all.size(); ++i) {
        if (!all[i].channelId.isValid())
            continue;
        if (i == activeIndex)
            newActive = valid.size();
        valid.append(all[i]);
    }

    settings.remove("layout/tabs");
    settings.beginWriteArray("layout/tabs");
    for (int i = 0; i < valid.size(); ++i) {
        settings.setArrayIndex(i);
        const TabEntry &entry = valid[i];
        settings.setValue("channelId", static_cast<quint64>(entry.channelId));
        settings.setValue("guildId", static_cast<quint64>(entry.guildId));
        settings.setValue("accountId", static_cast<quint64>(entry.accountId));
        settings.setValue("name", entry.name);
        settings.setValue("iconUrl", entry.iconUrl);
        settings.setValue("isDm", entry.isDm);
        settings.setValue("isForum", entry.isForum);
    }
    settings.endArray();
    settings.setValue("layout/activeTab", newActive);

    QStringList expanded, collapsed, collapsedCategories;
    QSet<QString> presentAccounts;
    captureTreeState(expanded, collapsed, collapsedCategories, presentAccounts);

    if (channelListMode == ChannelListMode::Tree) {
        settings.setValue("layout/tree/expanded",
                          mergeTreeKeys(settings.value("layout/tree/expanded").toStringList(),
                                        expanded,
                                        presentAccounts));
        settings.setValue("layout/tree/collapsed",
                          mergeTreeKeys(settings.value("layout/tree/collapsed").toStringList(),
                                        collapsed,
                                        presentAccounts));
    }
    settings.setValue("layout/tree/collapsedCategories",
                      mergeTreeKeys(settings.value("layout/tree/collapsedCategories").toStringList(),
                                    collapsedCategories,
                                    presentAccounts));

    settings.setValue("ui/channelListMode", channelListMode == ChannelListMode::Classic ? "classic" : "tree");
    if (serverRailModel)
        settings.setValue("ui/rail/expandedFolders", serverRailModel->expandedFolderKeys());

    settings.remove("ui/rail/lastChannels");
    settings.beginWriteArray("ui/rail/lastChannels");
    int lastChannelIdx = 0;
    for (auto it = lastViewedChannel.cbegin(); it != lastViewedChannel.cend(); ++it) {
        settings.setArrayIndex(lastChannelIdx++);
        settings.setValue("key", it.key());
        settings.setValue("channel", static_cast<qulonglong>(it.value()));
    }
    settings.endArray();
}

void MainWindow::restoreWindowState()
{
    QSettings settings;

    if (settings.contains("layout/geometry"))
        restoreGeometry(settings.value("layout/geometry").toByteArray());
    if (mainSplitter && settings.contains("layout/splitter"))
        mainSplitter->restoreState(settings.value("layout/splitter").toByteArray());

    const auto toSet = [](const QStringList &list) {
        return QSet<QString>(list.cbegin(), list.cend());
    };
    savedExpandedNodes = toSet(settings.value("layout/tree/expanded").toStringList());
    savedCollapsedNodes = toSet(settings.value("layout/tree/collapsed").toStringList());
    savedCollapsedCategories = toSet(settings.value("layout/tree/collapsedCategories").toStringList());
    hasSavedTreeState = !savedExpandedNodes.isEmpty() ||
                        !savedCollapsedNodes.isEmpty() ||
                        !savedCollapsedCategories.isEmpty();

    savedRailAccountId = Core::Snowflake(settings.value("ui/rail/lastAccountId").toULongLong());
    savedRailGuildId = Core::Snowflake(settings.value("ui/rail/lastGuildId").toULongLong());
    savedRailIsHome = settings.value("ui/rail/lastIsHome", true).toBool();
    hasSavedRailSelection = settings.contains("ui/rail/lastAccountId");
    if (serverRailModel)
        serverRailModel->setExpandedFolderKeys(settings.value("ui/rail/expandedFolders").toStringList());

    int lastChannelCount = settings.beginReadArray("ui/rail/lastChannels");
    for (int i = 0; i < lastChannelCount; ++i) {
        settings.setArrayIndex(i);
        QString key = settings.value("key").toString();
        if (!key.isEmpty())
            lastViewedChannel.insert(key, Core::Snowflake(settings.value("channel").toULongLong()));
    }
    settings.endArray();

    QList<TabEntry> entries;
    int count = settings.beginReadArray("layout/tabs");
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        TabEntry entry;
        entry.channelId = Core::Snowflake(settings.value("channelId").toULongLong());
        entry.guildId = Core::Snowflake(settings.value("guildId").toULongLong());
        entry.accountId = Core::Snowflake(settings.value("accountId").toULongLong());
        entry.name = settings.value("name").toString();
        entry.iconUrl = settings.value("iconUrl").toUrl();
        entry.isDm = settings.value("isDm").toBool();
        entry.isForum = settings.value("isForum").toBool();
        if (entry.channelId.isValid())
            entries.append(entry);
    }
    settings.endArray();

    if (!entries.isEmpty()) {
        int activeTab = qBound(0, settings.value("layout/activeTab", 0).toInt(), entries.size() - 1);
        tabBar->restoreTabs(entries, activeTab);
        // wait for READY
        pendingActiveEntry = entries[activeTab];
    }
}

void MainWindow::setupMenu()
{
    auto *menuBar = this->menuBar();
    QMenu *viewMenu = menuBar->addMenu(tr("&View"));
    auto *accountsAction = new QAction(tr("&Accounts"), this);
    connect(accountsAction, &QAction::triggered, this, &MainWindow::openAccountsWindow);
    viewMenu->addAction(accountsAction);

    auto *settingsAction = new QAction(tr("&Settings"), this);
    connect(settingsAction, &QAction::triggered, this, &MainWindow::openSettingsWindow);
    viewMenu->addAction(settingsAction);

    // DEBUG: Ctrl+Shift+R to force a Gateway reconnect
    auto *debugReconnect = new QAction(this);
    debugReconnect->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    addAction(debugReconnect);
    connect(debugReconnect, &QAction::triggered, this, [this]() {
        if (currentInstance)
            currentInstance->discord()->debugForceReconnect();
    });
}

void MainWindow::openAccountsWindow()
{
    if (!accountsWindow) {
        accountsWindow = new AccountsWindow(session, accountsModel);
    }

    accountsWindow->show();
    accountsWindow->raise();
    accountsWindow->activateWindow();
}

void MainWindow::openSettingsWindow()
{
    if (!settingsWindow) {
        settingsWindow = new SettingsWindow();
        connect(settingsWindow, &SettingsWindow::channelListModeChanged, this, [this](bool classic) {
            setChannelListMode(classic ? ChannelListMode::Classic : ChannelListMode::Tree);
        });
    }

    settingsWindow->show();
    settingsWindow->raise();
    settingsWindow->activateWindow();
}

void MainWindow::onTypingStart(const Discord::TypingStart &event)
{
    // todo im not really sure if i like having this handled here but its not that complex so oh well maybe later
    if (!currentInstance)
        return;

    if (event.member.hasValue() && event.guildId.hasValue()) {
        currentInstance->users()->saveMemberWithUser(event.guildId.get(), event.member.get());
    }

    std::optional<Snowflake> guildId =
            event.guildId.hasValue() ? std::optional(event.guildId.get()) : std::nullopt;

    typingTracker->addTyper(event.channelId.get(), event.userId.get(), guildId);
}

void MainWindow::onChannelPermissionsChanged(Core::Snowflake channelId)
{
    if (chatModel->getActiveChannelId() != channelId)
        return;

    if (!currentInstance)
        return;

    Core::Snowflake userId = currentInstance->accountId();
    bool canSend = currentInstance->permissions()->hasChannelPermission(
            userId, channelId, Discord::Permission::SEND_MESSAGES);
    bool canPin = currentInstance->permissions()->hasChannelPermission(
            userId, channelId, Discord::Permission::PIN_MESSAGES);
    bool canManage = currentInstance->permissions()->hasChannelPermission(
            userId, channelId, Discord::Permission::MANAGE_MESSAGES);

    int rateLimit = currentInstance->getChannelRateLimit(channelId);
    bool canBypass = currentInstance->permissions()->hasChannelPermission(
            userId, channelId, Discord::Permission::BYPASS_SLOWMODE);
    slowModeIndicator->setSlowMode(channelId, rateLimit, canBypass);

    bool onCooldown = rateLimit > 0 && !canBypass && slowModeIndicator->isOnCooldown(channelId);
    messageInput->setEnabled(canSend);
    messageInput->setSendBlocked(onCooldown);
    chatView->setCanPinMessages(canPin);
    chatView->setCanManageMessages(canManage);

    if (!canSend) {
        messageInput->setPlaceholder("You do not have permission to send messages");
    } else if (onCooldown) {
        messageInput->setPlaceholder("Slowmode is active");
    } else {
        QString channelName;
        QModelIndex current = channelTree->currentIndex();
        if (current.isValid()) {
            auto *node = channelTreeModel->nodeFromIndex(channelFilterProxy->mapToSource(current));
            if (node)
                channelName = node->name;
        }
        if (channelName.isEmpty())
            channelName = tabBar->activeTabName();
        if (!channelName.isEmpty())
            messageInput->setPlaceholder("Message #" + channelName);
    }
}

QColor MainWindow::resolveRoleColor(Snowflake userId, Snowflake guildId)
{
    if (!currentInstance || guildId == Snowflake::Invalid)
        return QColor();

    if (cachedGuildId == guildId && userColorCache.contains(userId))
        return userColorCache.value(userId);

    QColor result;
    for (const auto &role : currentInstance->getMemberRolesSorted(guildId, userId)) {
        if (role.hasColor()) {
            result = role.getColor();
            break;
        }
    }

    if (cachedGuildId == guildId)
        userColorCache[userId] = result;

    return result;
}

void MainWindow::refreshGuildRoleData(Snowflake guildId)
{
    channelTreeModel->invalidateGuildData(guildId);

    if (cachedGuildId == guildId) {
        userColorCache.clear();
        chatModel->refreshUsersInView({});
        forumModel->refreshAuthors();
    }
}

namespace {

QWidget *buildUserMenuHeader(QMenu *parent, Core::Session *session, Snowflake userId,
                             const QString &displayName, const QString &username,
                             const QString &avatarHash)
{
    auto *header = new QWidget(parent);
    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(10, 10, 16, 10);
    layout->setSpacing(12);

    constexpr QSize avatarSize(64, 64);
    auto *avatar = new QLabel(header);
    avatar->setFixedSize(avatarSize);
    QUrl url = Discord::Cdn::userAvatar(userId, avatarHash, 128);
    session->getImageManager()->assign(avatar, url, avatarSize);
    layout->addWidget(avatar);

    auto *textLayout = new QVBoxLayout;
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);

    auto *displayLabel = new QLabel(displayName, header);
    QFont displayFont = displayLabel->font();
    displayFont.setBold(true);
    displayFont.setPointSize(displayFont.pointSize() + 2);
    displayLabel->setFont(displayFont);
    textLayout->addWidget(displayLabel);

    if (!username.isEmpty() && username != displayName) {
        auto *usernameLabel = new QLabel(username, header);
        usernameLabel->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
        textLayout->addWidget(usernameLabel);
    }
    textLayout->addStretch();
    layout->addLayout(textLayout, 1);

    return header;
}

QWidget *buildRoleChip(const Discord::Role &role, QWidget *parent)
{
    auto *label = new QLabel(role.name.get(), parent);
    label->setContentsMargins(24, 4, 24, 4);
    if (role.hasColor())
        label->setStyleSheet(QStringLiteral("color: %1;").arg(role.getColor().name()));
    return label;
}

} // namespace

void MainWindow::showUserContextMenu(Snowflake userId, Snowflake guildId, QPoint globalPos)
{
    QMenu menu(this);

    if (currentInstance) {
        auto user = currentInstance->users()->getUser(userId);
        QString displayName = currentInstance->users()->getDisplayName(userId, guildId);
        QString username = (user && user->username.hasValue()) ? user->username.get() : QString();
        QString avatarHash = (user && user->avatar.hasValue()) ? user->avatar.get() : QString();

        auto *header = buildUserMenuHeader(&menu, session, userId, displayName, username,
                                           avatarHash);
        auto *headerAction = new QWidgetAction(&menu);
        headerAction->setDefaultWidget(header);
        menu.addAction(headerAction);
        menu.addSeparator();
    }

    QAction *profileAction = menu.addAction(tr("Profile"));
    connect(profileAction, &QAction::triggered, this, [this, userId, guildId]() {
        (new UserProfilePopup(session->getImageManager(), currentInstance, userId, guildId,
                              this))
                ->show();
    });

    QAction *mentionAction = menu.addAction(tr("Mention"));
    connect(mentionAction, &QAction::triggered, this, [this, userId]() {
        messageInput->insertText(QStringLiteral("<@%1>").arg(quint64(userId)));
    });

    QAction *openDmAction = menu.addAction(tr("Open DM"));
    std::optional<Snowflake> dmChannelId;
    if (currentInstance)
        dmChannelId = currentInstance->findDmChannelWithUser(userId);
    if (dmChannelId.has_value()) {
        connect(openDmAction, &QAction::triggered, this,
                [this, channelId = *dmChannelId]() { selectChannelInTree(channelId); });
    } else {
        openDmAction->setEnabled(false);
    }

    if (guildId.isValid() && currentInstance) {
        QMenu *rolesMenu = menu.addMenu(tr("Roles"));
        const auto memberRoles = currentInstance->getMemberRolesSorted(guildId, userId);
        if (memberRoles.isEmpty()) {
            rolesMenu->addAction(tr("No roles"))->setEnabled(false);
        } else {
            for (const auto &role : memberRoles) {
                auto *action = new QWidgetAction(rolesMenu);
                action->setDefaultWidget(buildRoleChip(role, rolesMenu));
                rolesMenu->addAction(action);
            }
        }
    }

    menu.exec(globalPos);
}

void MainWindow::selectChannelInTree(Snowflake channelId)
{
    if (!currentInstance)
        return;
    ChannelNode *node = channelTreeModel->findChannelTreeNode(channelId,
                                                              currentInstance->accountId());
    if (!node)
        return;

    if (channelListMode == ChannelListMode::Classic) {
        Snowflake accountId = currentInstance->accountId();
        if (node->type == ChannelNode::Type::DMChannel) {
            onRailAccountHomeSelected(accountId);
        } else {
            ChannelNode *guild = ChannelTreeModel::findGuildNode(node);
            if (guild)
                onRailGuildSelected(accountId, guild->id);
        }
    }

    QModelIndex sourceIndex = channelTreeModel->indexForNode(node);
    if (!sourceIndex.isValid())
        return;
    QModelIndex proxyIndex = channelFilterProxy->mapFromSource(sourceIndex);
    if (proxyIndex.isValid())
        channelTree->setCurrentIndex(proxyIndex);
}

void MainWindow::setThreadBrowserTarget(Core::Snowflake channelId)
{
    threadBrowserChannelId = channelId;
    channelToolbar->setVisible(channelId.isValid());
}

void MainWindow::openThreadBrowser()
{
    if (!currentInstance || !threadBrowserChannelId.isValid())
        return;

    if (!threadBrowser) {
        threadBrowser = new ThreadBrowserPopup(this);
        connect(threadBrowser, &ThreadBrowserPopup::threadActivated, this,
                &MainWindow::navigateToChannel);
    }

    QString name;
    if (auto ch = currentInstance->getChannel(threadBrowserChannelId); ch && ch->name.hasValue())
        name = ch->name.get();

    threadBrowser->configure(currentInstance, threadBrowserChannelId, name);
    threadBrowser->show();
    threadBrowser->raise();
}

void MainWindow::navigateToChannel(Core::Snowflake channelId)
{
    if (!currentInstance)
        return;
    Core::Snowflake acc = currentInstance->accountId();

    if (!channelTreeModel->findChannelTreeNode(channelId, acc)) {
        auto chOpt = currentInstance->getChannel(channelId);
        if (chOpt && chOpt->isThread())
            channelTreeModel->showTemporaryThread(*chOpt, acc);
    }
    selectChannelInTree(channelId);
}

} // namespace UI
} // namespace Acheron
