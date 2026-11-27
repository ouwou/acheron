#pragma once

#include <QDialog>
#include <QPointer>

#include <functional>

#include "Core/Snowflake.hpp"

class QLabel;
class QVBoxLayout;

namespace Acheron {
namespace Core {
class ClientInstance;
}

namespace UI {

// Pending friend requests and message requests for one account, with accept/ignore buttons.
// Rows are rebuilt from the account's state whenever it changes; nothing is cached here.
class RequestsDialog : public QDialog
{
    Q_OBJECT
public:
    using OpenChannelFn = std::function<void(Core::Snowflake channelId)>;
    RequestsDialog(Core::ClientInstance *instance, OpenChannelFn openChannel, QWidget *parent = nullptr);

    static int pendingCount(Core::ClientInstance *instance);

private:
    void rebuild();
    void addSection(const QString &title);
    void addRow(const QString &name, const QString &detail, const QList<QPair<QString, std::function<void()>>> &actions);
    void runAction(const QString &what, const std::function<void(std::function<void(bool, const QString &)>)> &call);

    QPointer<Core::ClientInstance> instance;
    OpenChannelFn openChannel;
    QVBoxLayout *rows = nullptr;
    QLabel *status = nullptr;
};

} // namespace UI
} // namespace Acheron
