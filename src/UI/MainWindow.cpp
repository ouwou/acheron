#include "MainWindow.hpp"

#include "Chat/ChatModel.hpp"
#include "Chat/ChatDelegate.hpp"
#include "Chat/ChatView.hpp"
#include "ChannelList/ChannelTreeModel.hpp"
#include "ChannelList/ChannelFilterProxyModel.hpp"
#include "ChannelList/ChannelDelegate.hpp"
#include "ChannelList/ChannelTreeView.hpp"
#include "Accounts/AccountsWindow.hpp"
#include "Accounts/AccountsModel.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/AccountInfo.hpp"
#include "Core/UserManager.hpp"
#include "Core/TypingTracker.hpp"
#include "Core/Logging.hpp"
#include "Discord/Events.hpp"
#include "TypingIndicator.hpp"

using namespace Acheron::Core;

namespace Acheron {
namespace UI {

MainWindow::MainWindow(Session *session, QWidget *parent) : QMainWindow(parent), session(session)
{
    chatModel = new ChatModel(session->getImageManager(), session->getAttachmentCache(), this);
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

    chatModel->setRoleColorResolver([this](Snowflake userId, Snowflake guildId) -> QColor {
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
    });

    typingTracker = new TypingTracker(this);

    setupUi();
    setupMenu();

    connect(typingTracker, &TypingTracker::typersChanged, this,
            [this]() { typingIndicator->setTypers(typingTracker->getActiveTyperNames()); });

    connect(session, &Session::ready, this,
            [this](const Discord::Ready &ready) { channelTreeModel->populateFromReady(ready); });

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

    ClientInstance *selectedInstance = session->client(accountNode->id);
    if (!selectedInstance) {
        messageInput->setEnabled(false);
        return;
    }

    if (selectedInstance != currentInstance)
        switchActiveInstance(selectedInstance);

    Core::Snowflake userId = selectedInstance->accountId();
    Core::Snowflake channelId = node->id;

    if (node->type == ChannelNode::Type::DMChannel) {
        messageInput->setEnabled(true);
        messageInput->setPlaceholder("Message @" + node->name);
    } else {
        bool canSend = selectedInstance->permissions()->hasChannelPermission(
                userId, channelId, Discord::Permission::SEND_MESSAGES);

        messageInput->setEnabled(canSend);

        if (canSend)
            messageInput->setPlaceholder("Message #" + node->name);
        else
            messageInput->setPlaceholder("You do not have permission to send messages");

        selectedInstance->discord()->ensureSubscriptionByChannel(node->id);
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
    }

    messages->requestLoadChannel(node->id);
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
    }

    currentInstance = newInstance;
    auto *msgs = currentInstance->messages();

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
}

void MainWindow::setupPermanentConnections(Core::ClientInstance *instance)
{
    if (!instance)
        return;

    connect(instance, &Core::ClientInstance::channelCreated, this,
            [this, instance](const Discord::ChannelCreate &event) {
                channelTreeModel->addChannel(event, instance->accountId());
            });

    connect(instance, &Core::ClientInstance::channelUpdated, this,
            [this, instance](const Discord::ChannelUpdate &update) {
                channelTreeModel->updateChannel(update, instance->accountId());
            });

    connect(instance, &Core::ClientInstance::channelDeleted, this,
            [this, instance](const Discord::ChannelDelete &event) {
                channelTreeModel->deleteChannel(event, instance->accountId());
            });
}

void MainWindow::setupUi()
{
    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);

    channelTree = new ChannelTreeView(central);

    auto *rightSideWidget = new QWidget(central);
    auto *rightLayout = new QVBoxLayout(rightSideWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    chatView = new ChatView(rightSideWidget);
    messageInput = new MessageInput(rightSideWidget);
    typingIndicator = new TypingIndicator(rightSideWidget);

    rightLayout->addWidget(chatView, 1);
    rightLayout->addWidget(typingIndicator, 0);
    rightLayout->addWidget(messageInput, 0);

    auto *splitter = new QSplitter(this);
    splitter->addWidget(channelTree);
    splitter->addWidget(rightSideWidget);

    splitter->setCollapsible(0, false);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    channelTree->setMinimumWidth(200);

    channelTree->setModel(channelFilterProxy);
    channelTree->setHeaderHidden(true);
    channelTree->setIndentation(0);
    channelTree->setItemDelegate(new ChannelDelegate(channelFilterProxy, channelTree));
    channelTree->setIconSize(QSize(24, 24));
    channelTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    channelTree->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    channelTree->setExpandsOnDoubleClick(false);

    chatView->setModel(chatModel);
    chatView->setItemDelegate(new ChatDelegate(chatView));
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

        currentInstance->messages()->sendMessage(channelId, text);
    });

    connect(chatView, &ChatView::historyRequested, this, [this]() {
        Snowflake oldestId = chatModel->getOldestMessageId();

        if (currentInstance && oldestId.isValid())
            currentInstance->messages()->requestLoadHistory(chatModel->getActiveChannelId(),
                                                            oldestId);
    });

    connect(channelTree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MainWindow::onChannelSelectionChanged);

    layout->addWidget(splitter);
    layout->setContentsMargins(4, 0, 4, 0);
    setCentralWidget(central);
}

void MainWindow::setupMenu()
{
    auto *menuBar = this->menuBar();
    QMenu *viewMenu = menuBar->addMenu(tr("&View"));
    auto *accountsAction = new QAction(tr("&Accounts"), this);
    connect(accountsAction, &QAction::triggered, this, &MainWindow::openAccountsWindow);
    viewMenu->addAction(accountsAction);
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

    QModelIndex current = channelTree->currentIndex();
    if (!current.isValid())
        return;

    ChannelNode *node = channelTreeModel->nodeFromIndex(channelFilterProxy->mapToSource(current));
    if (!node || node->type != ChannelNode::Type::Channel)
        return;

    Core::Snowflake userId = currentInstance->accountId();
    bool canSend = currentInstance->permissions()->hasChannelPermission(
            userId, channelId, Discord::Permission::SEND_MESSAGES);

    messageInput->setEnabled(canSend);

    if (canSend)
        messageInput->setPlaceholder(node->name);
    else
        messageInput->setPlaceholder("You do not have permission to send messages");
}

} // namespace UI
} // namespace Acheron