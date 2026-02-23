#include "MainWindow.hpp"

#include "Chat/ChatModel.hpp"
#include "Chat/ChatDelegate.hpp"
#include "Chat/ChatView.hpp"
#include "ChannelList/ChannelTreeModel.hpp"
#include "ChannelList/ChannelFilterProxyModel.hpp"
#include "ChannelList/ChannelDelegate.hpp"
#include "ChannelList/ChannelTreeView.hpp"
#include "TabBar/TabBar.hpp"
#include "Accounts/AccountsWindow.hpp"
#include "Accounts/AccountsModel.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/AccountInfo.hpp"
#include "Core/UserManager.hpp"
#include "Core/TypingTracker.hpp"
#include "Core/Logging.hpp"
#include "Core/ReadStateManager.hpp"
#include "Discord/Events.hpp"
#include "TypingIndicator.hpp"
#include "SlowModeIndicator.hpp"
#include "ConnectionBanner.hpp"
#include "Dialogs/ConfirmPopup.hpp"
#include "Core/MemberListManager.hpp"
#include "Core/AV/VoiceManager.hpp"
#include "VoiceStatusBar.hpp"

using namespace Acheron::Core;

namespace Acheron {
namespace UI {

MainWindow::MainWindow(Session *session, QWidget *parent) : QMainWindow(parent), session(session)
{
    chatModel = new ChatModel(session->getImageManager(), this);
    channelTreeModel = new ChannelTreeModel(session, this);
    channelFilterProxy = new ChannelFilterProxyModel(session, this);
    channelFilterProxy->setSourceModel(channelTreeModel);
    channelFilterProxy->setDynamicSortFilter(true);
    channelFilterProxy->sort(0);
    accountsModel = new AccountsModel(session, this);

    chatModel->setAvatarUrlResolver([this](const Discord::User &user) -> QUrl {
        // using a resolver cuz spacebar ig
        return QString("https://cdn.discordapp.com/avatars/%1/%2.png?size=64")
                .arg(quint64(user.id.get()))
                .arg(user.avatar.get());
    });

    chatModel->setDisplayNameResolver([this](Snowflake userId, Snowflake guildId) -> QString {
        if (!currentInstance)
            return QString();
        return currentInstance->users()->getDisplayName(userId,
                                                        guildId != Snowflake::Invalid
                                                                ? std::optional(guildId)
                                                                : std::nullopt);
    });

    chatModel->setRoleColorResolver(
            [this](Snowflake userId, Snowflake guildId) { return resolveRoleColor(userId, guildId); });

    typingTracker = new TypingTracker(this);
    memberListModel = new MemberListModel(session->getImageManager(), this);

    setupUi();
    setupMenu();

    qApp->installEventFilter(this);

    typingIndicator->setRoleColorResolver(
            [this](Snowflake userId, Snowflake guildId) { return resolveRoleColor(userId, guildId); });

    connect(typingTracker, &TypingTracker::typersChanged, this,
            [this]() { typingIndicator->setTypers(typingTracker->getActiveTypers()); });

    connect(session, &Session::ready, this, [this](const Discord::Ready &ready) {
        channelTreeModel->populateFromReady(ready);
        channelTree->performDefaultExpansion();
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
                        channelTreeModel->removeAccount(acc.id);
                    }
                }
            });

    for (const auto &instance : session->getClients()) {
        if (instance)
            setupPermanentConnections(instance);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
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

    if (!node || (node->type != ChannelNode::Type::Channel &&
                  node->type != ChannelNode::Type::DMChannel))
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

    selectedInstance->readState()->setActiveChannel(node->id);

    Core::Snowflake userId = selectedInstance->accountId();
    Core::Snowflake channelId = node->id;

    if (node->type == ChannelNode::Type::DMChannel) {
        messageInput->setEnabled(true);
        messageInput->setSendBlocked(false);
        messageInput->setPlaceholder("Message @" + node->name);
        chatView->setCanPinMessages(true);
        chatView->setCanManageMessages(false);
        slowModeIndicator->setSlowMode(channelId, 0, false);
        memberListView->hide();
        selectedInstance->memberList()->clear();
    } else {
        bool canSend = selectedInstance->permissions()->hasChannelPermission(
                userId, channelId, Discord::Permission::SEND_MESSAGES);
        bool canPin = selectedInstance->permissions()->hasChannelPermission(
                userId, channelId, Discord::Permission::PIN_MESSAGES);
        bool canManage = selectedInstance->permissions()->hasChannelPermission(
                userId, channelId, Discord::Permission::MANAGE_MESSAGES);

        int rateLimit = selectedInstance->getChannelRateLimit(channelId);
        bool canBypass = selectedInstance->permissions()->hasChannelPermission(
                userId, channelId, Discord::Permission::BYPASS_SLOWMODE);
        slowModeIndicator->setSlowMode(channelId, rateLimit, canBypass);

        bool onCooldown = rateLimit > 0 && !canBypass && slowModeIndicator->isOnCooldown(channelId);
        messageInput->setEnabled(canSend);
        messageInput->setSendBlocked(onCooldown);
        chatView->setCanPinMessages(canPin);
        chatView->setCanManageMessages(canManage);

        if (!canSend)
            messageInput->setPlaceholder("You do not have permission to send messages");
        else if (onCooldown)
            messageInput->setPlaceholder("Slowmode is active");
        else
            messageInput->setPlaceholder("Message #" + node->name);

        ChannelNode *gNode = node;
        while (gNode && gNode->type != ChannelNode::Type::Server)
            gNode = gNode->parent;
        Snowflake gId = gNode ? gNode->id : Snowflake::Invalid;

        if (gId.isValid()) {
            memberListView->show();
            selectedInstance->memberList()->setActiveChannel(gId, node->id);
        }
    }

    MessageManager *messages = selectedInstance->messages();

    if (node->id != chatModel->getActiveChannelId()) {
        ChannelNode *guildNode = node;
        while (guildNode && guildNode->type != ChannelNode::Type::Server)
            guildNode = guildNode->parent;
        Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;

        if (guildId != cachedGuildId) {
            cachedGuildId = guildId;
            userColorCache.clear();
        }

        chatModel->setActiveChannel(node->id, guildId);
        typingTracker->setActiveChannel(node->id);
        messageInput->clearReplyTarget();
    }

    messages->requestLoadChannel(node->id);

    if (node->isUnread && node->lastMessageId.isValid())
        selectedInstance->readState()->markChannelAsRead(node->id, node->lastMessageId);

    {
        ChannelNode *serverNode = node;
        while (serverNode && serverNode->type != ChannelNode::Type::Server)
            serverNode = serverNode->parent;

        TabEntry entry;
        entry.channelId = node->id;
        entry.guildId = serverNode ? serverNode->id : Snowflake::Invalid;
        entry.accountId = accountNode->id;
        entry.name = node->name;
        entry.isDm = (node->type == ChannelNode::Type::DMChannel);
        if (serverNode && !serverNode->TEMP_iconHash.isEmpty())
            entry.iconUrl = QStringLiteral("https://cdn.discordapp.com/icons/%1/%2.png?size=64")
                                    .arg(quint64(serverNode->id))
                                    .arg(serverNode->TEMP_iconHash);
        tabBar->updateCurrentTab(entry);
    }
}

void MainWindow::switchActiveInstance(Core::ClientInstance *newInstance)
{
    if (currentInstance) {
        auto *msgs = currentInstance->messages();
        disconnect(msgs, nullptr, chatModel, nullptr);
        disconnect(msgs, nullptr, this, nullptr);
        disconnect(currentInstance->discord(), &Discord::Client::typingStart, this, nullptr);
        disconnect(currentInstance->permissions(), nullptr, this, nullptr);
        disconnect(currentInstance, &Core::ClientInstance::membersUpdated, this, nullptr);
        disconnect(memberListView, nullptr, currentInstance->memberList(), nullptr);
    }

    currentInstance = newInstance;
    auto *msgs = currentInstance->messages();

    memberListModel->setManager(currentInstance->memberList());
    connect(memberListView, &MemberListView::visibleRangeChanged,
            currentInstance->memberList(), &Core::MemberListManager::updateSubscriptionRange);

    chatView->setCurrentUserId(currentInstance->accountId());

    typingTracker->clear();
    typingTracker->setUserManager(currentInstance->users());
    typingTracker->setCurrentUserId(currentInstance->accountId());

    connect(msgs, &MessageManager::messagesReceived, chatModel, &ChatModel::handleIncomingMessages);
    connect(msgs, &MessageManager::messageErrored, chatModel, &ChatModel::handleMessageErrored);
    connect(msgs, &MessageManager::messageDeleted, chatModel, &ChatModel::handleMessageDeleted);
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
            });

    updateVoiceStatusLabel();
}

void MainWindow::setupPermanentConnections(Core::ClientInstance *instance)
{
    if (!instance)
        return;

    if (instancesSignalsConnected.contains(instance->accountId()))
        return;
    instancesSignalsConnected.insert(instance->accountId());

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

    // todo: i dont really like the refresh users logic rn
    connect(instance, &Core::ClientInstance::guildRoleCreated, this,
            [this, instance](const Discord::GuildRoleCreate &event) {
                if (!event.guildId.hasValue())
                    return;

                Core::Snowflake guildId = event.guildId.get();
                guildRolesCache.remove(guildId);

                channelTreeModel->invalidateGuildData(guildId);

                if (cachedGuildId == guildId) {
                    userColorCache.clear();
                    chatModel->refreshUsersInView({});
                }
            });

    connect(instance, &Core::ClientInstance::guildRoleUpdated, this,
            [this, instance](const Discord::GuildRoleUpdate &event) {
                if (!event.guildId.hasValue())
                    return;

                Core::Snowflake guildId = event.guildId.get();
                guildRolesCache.remove(guildId);

                channelTreeModel->invalidateGuildData(guildId);

                if (cachedGuildId == guildId) {
                    userColorCache.clear();
                    chatModel->refreshUsersInView({});
                }
            });

    connect(instance, &Core::ClientInstance::guildRoleDeleted, this,
            [this, instance](const Discord::GuildRoleDelete &event) {
                if (!event.guildId.hasValue())
                    return;

                Core::Snowflake guildId = event.guildId.get();
                guildRolesCache.remove(guildId);

                channelTreeModel->invalidateGuildData(guildId);

                if (cachedGuildId == guildId) {
                    userColorCache.clear();
                    chatModel->refreshUsersInView({});
                }
            });

    connect(instance, &Core::ClientInstance::readStateChanged, this,
            [this, instance](Core::Snowflake channelId) {
                channelTreeModel->updateReadState(channelId, instance->accountId());
                refreshTabReadStates();
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

    connect(instance, &Core::ClientInstance::voiceStateChanged, this,
            [this, instance](Core::Snowflake channelId, Core::Snowflake) {
                channelTree->setAccountVoiceChannel(instance->accountId(), channelId);
                updateVoiceStatusLabel();
            });

    connect(instance->voice(), &Core::AV::VoiceManager::voiceStateChanged,
            this, &MainWindow::updateVoiceStatusLabel);
}

void MainWindow::setupUi()
{
    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);

    auto *leftSideWidget = new QWidget(central);
    auto *leftLayout = new QVBoxLayout(leftSideWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    channelTree = new ChannelTreeView(leftSideWidget);

    voiceStatusBar = new VoiceStatusBar(leftSideWidget);
    voiceStatusBar->setImageManager(session->getImageManager());
    connect(voiceStatusBar, &VoiceStatusBar::disconnectRequested, this, [this]() {
        for (const auto &inst : session->getClients()) {
            if (inst && inst->isInVoice()) {
                inst->discord()->sendVoiceStateUpdate(inst->voiceGuildId(), Snowflake::Invalid, false, false);
                break;
            }
        }
    });

    leftLayout->addWidget(channelTree, 1);
    leftLayout->addWidget(voiceStatusBar, 0);

    auto *rightSideWidget = new QWidget(central);
    auto *rightLayout = new QVBoxLayout(rightSideWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    connectionBanner = new ConnectionBanner(rightSideWidget);
    tabBar = new TabBar(session->getImageManager(), rightSideWidget);
    chatView = new ChatView(rightSideWidget);
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

    rightLayout->addWidget(connectionBanner, 0);
    rightLayout->addWidget(tabBar, 0);
    rightLayout->addWidget(chatView, 1);
    rightLayout->addWidget(statusRow, 0);
    rightLayout->addWidget(messageInput, 0);

    memberListView = new MemberListView(central);
    memberListView->setModel(memberListModel);
    memberListView->setItemDelegate(new MemberListDelegate(memberListView));

    mainSplitter = new QSplitter(this);
    mainSplitter->addWidget(leftSideWidget);
    mainSplitter->addWidget(rightSideWidget);
    mainSplitter->addWidget(memberListView);

    mainSplitter->setCollapsible(0, false);
    mainSplitter->setCollapsible(2, false);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setStretchFactor(2, 0);
    leftSideWidget->setMinimumWidth(200);
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

                ClientInstance *instance = session->client(accountNode->id);
                if (!instance)
                    return;

                auto pairs = channelTreeModel->getMarkableChannels(sourceIndex);

                if (pairs.size() == 1)
                    instance->readState()->markChannelAsRead(pairs.first().first, pairs.first().second);
                else if (!pairs.isEmpty())
                    instance->readState()->markChannelsAsRead(pairs);
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

    connect(messageInput, &MessageInput::sendMessage, this, [this](const QString &text) {
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
        currentInstance->messages()->sendMessage(channelId, text, replyTo);

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

                ChannelNode *guildNode = node;
                while (guildNode && guildNode->type != ChannelNode::Type::Server)
                    guildNode = guildNode->parent;
                Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;

                TabEntry entry;
                entry.channelId = node->id;
                entry.guildId = guildId;
                entry.accountId = accountNode->id;
                entry.name = node->name;
                entry.isDm = (node->type == ChannelNode::Type::DMChannel);
                if (guildNode && !guildNode->TEMP_iconHash.isEmpty())
                    entry.iconUrl = QStringLiteral("https://cdn.discordapp.com/icons/%1/%2.png?size=64")
                                            .arg(quint64(guildNode->id))
                                            .arg(guildNode->TEMP_iconHash);
                tabBar->openNewTab(entry);
            });

    connect(channelTree, &ChannelTreeView::joinVoiceChannelRequested, this,
            [this](const QModelIndex &proxyIndex) {
                QModelIndex sourceIndex = channelFilterProxy->mapToSource(proxyIndex);
                auto *node = channelTreeModel->nodeFromIndex(sourceIndex);
                if (!node || node->type != ChannelNode::Type::VoiceChannel)
                    return;

                ChannelNode *accountNode = channelTreeModel->getAccountNodeFor(node);
                if (!accountNode)
                    return;

                ClientInstance *instance = session->client(accountNode->id);
                if (!instance)
                    return;

                ChannelNode *guildNode = node;
                while (guildNode && guildNode->type != ChannelNode::Type::Server)
                    guildNode = guildNode->parent;
                if (!guildNode)
                    return;

                qCInfo(LogVoice) << "Joining voice channel" << node->name << node->id
                                 << "in guild" << guildNode->id;
                instance->discord()->sendVoiceStateUpdate(guildNode->id, node->id, false, false);
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
                if (!instance)
                    return;

                ChannelNode *guildNode = node;
                while (guildNode && guildNode->type != ChannelNode::Type::Server)
                    guildNode = guildNode->parent;
                if (!guildNode)
                    return;

                qCInfo(LogVoice) << "Disconnecting from voice in guild" << guildNode->id;
                instance->discord()->sendVoiceStateUpdate(guildNode->id, Snowflake::Invalid, false, false);
            });

    layout->addWidget(mainSplitter);
    layout->setContentsMargins(0, 0, 4, 0);
    setCentralWidget(central);
}

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
            return um->getDisplayName(userId, vGuildId.isValid() ? std::optional(vGuildId) : std::nullopt);
        });
        voiceStatusBar->setAvatarResolver([um](Core::Snowflake userId) -> QUrl {
            if (!um)
                return {};
            Discord::User *user = um->getUser(userId);
            if (!user || user->avatar.isNull() || user->avatar.get().isEmpty())
                return {};
            return QUrl(QStringLiteral("https://cdn.discordapp.com/avatars/%1/%2.png?size=32")
                                .arg(quint64(userId))
                                .arg(user->avatar.get()));
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
            channelName = node ? node->name : QString::number(vcId);
        }
    }
    voiceStatusBar->setChannelName(channelName);
}

void MainWindow::switchToTabEntry(const TabEntry &entry)
{
    if (!entry.channelId.isValid())
        return;

    activateChannel(entry);
}

void MainWindow::activateChannel(const TabEntry &entry)
{
    // update the proxy selected channel so the delegate highlights correctly,
    // and clear the trees own selection so no stale highlight remains
    channelFilterProxy->setSelectedChannel(entry.channelId, entry.accountId);
    {
        QSignalBlocker blocker(channelTree->selectionModel());
        channelTree->selectionModel()->clearSelection();
        channelTree->selectionModel()->clearCurrentIndex();
    }
    channelTree->viewport()->update();

    ClientInstance *instance = session->client(entry.accountId);
    if (!instance) {
        messageInput->setEnabled(false);
        return;
    }

    if (instance != currentInstance)
        switchActiveInstance(instance);

    instance->readState()->setActiveChannel(entry.channelId);

    Snowflake userId = instance->accountId();

    if (entry.isDm) {
        messageInput->setEnabled(true);
        messageInput->setSendBlocked(false);
        messageInput->setPlaceholder("Message @" + entry.name);
        chatView->setCanPinMessages(true);
        chatView->setCanManageMessages(false);
        slowModeIndicator->setSlowMode(entry.channelId, 0, false);
        memberListView->hide();
        instance->memberList()->clear();
    } else {
        bool canSend = instance->permissions()->hasChannelPermission(userId, entry.channelId, Discord::Permission::SEND_MESSAGES);
        bool canPin = instance->permissions()->hasChannelPermission(userId, entry.channelId, Discord::Permission::PIN_MESSAGES);
        bool canManage = instance->permissions()->hasChannelPermission(userId, entry.channelId, Discord::Permission::MANAGE_MESSAGES);

        int rateLimit = instance->getChannelRateLimit(entry.channelId);
        bool canBypass = instance->permissions()->hasChannelPermission(
                userId, entry.channelId, Discord::Permission::BYPASS_SLOWMODE);
        slowModeIndicator->setSlowMode(entry.channelId, rateLimit, canBypass);

        bool onCooldown = rateLimit > 0 && !canBypass && slowModeIndicator->isOnCooldown(entry.channelId);
        messageInput->setEnabled(canSend);
        messageInput->setSendBlocked(onCooldown);
        chatView->setCanPinMessages(canPin);
        chatView->setCanManageMessages(canManage);

        if (!canSend)
            messageInput->setPlaceholder("You do not have permission to send messages");
        else if (onCooldown)
            messageInput->setPlaceholder("Slowmode is active");
        else
            messageInput->setPlaceholder("Message #" + entry.name);

        if (entry.guildId.isValid()) {
            memberListView->show();
            instance->memberList()->setActiveChannel(entry.guildId, entry.channelId);
        }
    }

    if (entry.channelId != chatModel->getActiveChannelId()) {
        if (entry.guildId != cachedGuildId) {
            cachedGuildId = entry.guildId;
            userColorCache.clear();
        }

        chatModel->setActiveChannel(entry.channelId, entry.guildId);
        typingTracker->setActiveChannel(entry.channelId);
        messageInput->clearReplyTarget();
    }

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
                entry.channelId, entry.guildId, entry.isDm);
        tabBar->updateChannelReadState(entry.channelId, state.isUnread, state.mentionCount);
    }
}

void MainWindow::setupMenu()
{
    auto *menuBar = this->menuBar();
    QMenu *viewMenu = menuBar->addMenu(tr("&View"));
    auto *accountsAction = new QAction(tr("&Accounts"), this);
    connect(accountsAction, &QAction::triggered, this, &MainWindow::openAccountsWindow);
    viewMenu->addAction(accountsAction);

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

    if (!guildRolesCache.contains(guildId))
        guildRolesCache[guildId] = currentInstance->getRolesForGuild(guildId);

    const auto &roles = guildRolesCache[guildId];

    Discord::Member *member = currentInstance->users()->getMember(guildId, userId);
    if (!member || !member->roles.hasValue())
        return QColor();

    int highestPos = -1;
    int colorValue = 0;
    for (const auto &roleId : member->roles.get()) {
        for (const auto &role : roles) {
            if (role.id == roleId && role.color.hasValue() && role.color.get() != 0) {
                if (role.position.get() > highestPos) {
                    highestPos = role.position.get();
                    colorValue = role.color.get();
                }
            }
        }
    }

    QColor result = colorValue != 0 ? QColor::fromRgb(colorValue) : QColor();

    if (cachedGuildId == guildId)
        userColorCache[userId] = result;

    return result;
}

} // namespace UI
} // namespace Acheron