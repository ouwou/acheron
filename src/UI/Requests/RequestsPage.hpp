#pragma once

#include <QPointer>
#include <QWidget>

#include <functional>

#include "Core/Snowflake.hpp"

class QLabel;
class QVBoxLayout;

namespace Acheron {
namespace Core {
class ClientInstance;
}

namespace UI {

// Pending friend requests and message requests for one account, shown in the chat area when the
// Requests row under Direct Messages is selected. Rows are rebuilt from the account's state only
// while the page is visible; nothing is cached here.
class RequestsPage : public QWidget
{
    Q_OBJECT
public:
    explicit RequestsPage(QWidget *parent = nullptr);

    void setInstance(Core::ClientInstance *instance);

signals:
    void openChannelRequested(Core::Snowflake channelId);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void rebuild();
    void addSection(const QString &title);
    void addRow(const QString &name, const QString &detail, const QList<QPair<QString, std::function<void()>>> &actions);
    void runAction(const QString &what, const std::function<void(std::function<void(bool, const QString &)>)> &call);

    QPointer<Core::ClientInstance> instance;
    QList<QMetaObject::Connection> connections;
    QVBoxLayout *rows = nullptr;
    QLabel *status = nullptr;
};

} // namespace UI
} // namespace Acheron
