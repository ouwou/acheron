#include "MainWindow.hpp"

#include "Chat/ChatModel.hpp"
#include "Chat/ChatDelegate.hpp"
#include "Chat/ChatView.hpp"
#include "ChannelList/ChannelTreeModel.hpp"
#include "ChannelList/ChannelDelegate.hpp"
#include "AccountsWindow.hpp"
#include "AccountsModel.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/AccountInfo.hpp"
#include "Core/UserManager.hpp"
#include "Core/TypingTracker.hpp"
#include "Discord/Events.hpp"
#include "TypingIndicator.hpp"

using namespace Acheron::Core;

namespace Acheron {
namespace UI {

MainWindow::MainWindow(Session *session, QWidget *parent) : QMainWindow(parent), session(session)
{
    chatModel = new ChatModel(session->getImageManager(), session->getAttachmentCache(), this);
    channelTreeModel = new ChannelTreeModel(session, this);
    accountsModel = new AccountsModel(session, this);

    chatModel->setAvatarUrlResolver([this](const Discord::User &user) -> QUrl {
        // using a resolver cuz spacebar ig
        return QString("https://cdn.discordapp.com/avatars/%1/%2.png?size=64")
                .arg(quint64(user.id.get()))
                .arg(user.avatar.get());
    });

    typingTracker = new TypingTracker(this);

    setupUi();
    setupMenu();

    connect(typingTracker, &TypingTracker::typersChanged, this,
            [this]() { typingIndicator->setTypers(typingTracker->getActiveTyperNames()); });

    connect(session, &Session::ready, this,
            [this](const Discord::Ready &ready) { channelTreeModel->populateFromReady(ready); });

    connect(accountsModel, &AccountsModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight,
                   const QVector<int> &roles) {
                for (int row = topLeft.row(); row <= bottomRight.row(); ++row) {
                    QModelIndex idx = accountsModel->index(row, 0);
                    auto accPtr = idx.data(AccountsModel::AccountObjectRole).value<void *>();

                    Acheron::Core::AccountInfo acc =
                            *static_cast<Acheron::Core::AccountInfo *>(accPtr);

                    if (acc.state == Acheron::Core::ConnectionState::Connected) {
                        channelTreeModel->addAccount(acc);

                        for (int i = 0; i < channelTreeModel->rowCount(QModelIndex()); ++i) {
                            QModelIndex treeIdx = channelTreeModel->index(i, 0, QModelIndex());
                            if (treeIdx.data(Qt::UserRole).toULongLong() == acc.id) {
                                channelTree->expand(treeIdx);
                                break;
                            }
                        }
                    } else if (acc.state == Acheron::Core::ConnectionState::Disconnected) {
                        channelTreeModel->removeAccount(acc.id);
                    }
                }
            });
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

    auto node = static_cast<ChannelNode *>(current.internalPointer());

    if (!node || node->type != ChannelNode::Type::Channel) {
        messageInput->setEnabled(false);
        messageInput->setPlaceholder("");
        return;
    }

    messageInput->setEnabled(true);
    messageInput->setPlaceholder(node->name);

    ChannelNode *accountNode = channelTreeModel->getAccountNodeFor(node);
    if (!accountNode)
        return;

    ClientInstance *selectedInstance = session->client(accountNode->id);
    if (!selectedInstance)
        return;

    if (selectedInstance != currentInstance)
        switchActiveInstance(selectedInstance);

    selectedInstance->discord()->ensureSubscriptionByChannel(node->id);

    MessageManager *messages = selectedInstance->messages();

    if (node->type == ChannelNode::Type::Channel) {
        if (node->id != chatModel->getActiveChannelId()) {
            chatModel->setActiveChannel(node->id);
            typingTracker->setActiveChannel(node->id);
        }
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
    }

    currentInstance = newInstance;
    auto *msgs = currentInstance->messages();

    typingTracker->clear();
    typingTracker->setUserManager(currentInstance->users());
    typingTracker->setCurrentUserId(currentInstance->accountId());

    connect(msgs, &MessageManager::messagesReceived, chatModel, &ChatModel::handleIncomingMessages);
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
}

void MainWindow::setupUi()
{
    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);

    channelTree = new QTreeView(central);

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

    channelTree->setModel(channelTreeModel);
    channelTree->setHeaderHidden(true);
    channelTree->setIndentation(0);
    channelTree->setItemDelegate(new ChannelDelegate(channelTree));
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

    connect(messageInput, &MessageInput::sendMessage, this,
            [this](const QString &text) { qDebug() << "meow: " << text; });

    connect(chatView, &ChatView::historyRequested, this, [this]() {
        Snowflake oldestId = chatModel->getOldestMessageId();

        if (currentInstance && oldestId.isValid())
            currentInstance->messages()->requestLoadHistory(chatModel->getActiveChannelId(),
                                                            oldestId);
    });

    connect(channelTree, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        if (!index.isValid())
            return;

        auto node = static_cast<ChannelNode *>(index.internalPointer());

        if (node->type == ChannelNode::Type::Category || node->type == ChannelNode::Type::Server ||
            node->type == ChannelNode::Type::Account || node->type == ChannelNode::Type::DMHeader ||
            node->type == ChannelNode::Type::Folder) {
            bool isExpanded = channelTree->isExpanded(index);
            channelTree->setExpanded(index, !isExpanded);
        }
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

} // namespace UI
} // namespace Acheron