#pragma once
#include <QtWidgets>
#include "Input/MessageInput.hpp"

namespace Acheron {
namespace Core {
class Session;
class ClientInstance;
} // namespace Core
namespace UI {
class ChatView;
class ChatModel;
class ChannelTreeModel;
class AccountsWindow;
class AccountsModel;
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

private slots:
    void onChannelSelectionChanged(const QModelIndex &current, const QModelIndex &previous);

private:
    void switchActiveInstance(Core::ClientInstance *instance);

private:
    void setupUi();
    void setupMenu();

    ChatView *chatView;
    ChatModel *chatModel;

    QTreeView *channelTree;
    ChannelTreeModel *channelTreeModel;

    AccountsModel *accountsModel;

    MessageInput *messageInput;

    AccountsWindow *accountsWindow = nullptr;

private slots:
    void openAccountsWindow();

private:
    Core::Session *session;
    Core::ClientInstance *currentInstance = nullptr;
};

} // namespace UI
} // namespace Acheron