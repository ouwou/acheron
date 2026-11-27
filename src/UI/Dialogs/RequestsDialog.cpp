#include "RequestsDialog.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "Core/ClientInstance.hpp"
#include "Core/RelationshipManager.hpp"
#include "Core/UserManager.hpp"
#include "Discord/Client.hpp"

namespace Acheron {
namespace UI {

RequestsDialog::RequestsDialog(Core::ClientInstance *instance, OpenChannelFn openChannel, QWidget *parent)
    : QDialog(parent), instance(instance), openChannel(std::move(openChannel))
{
    setWindowTitle(tr("Friend and Message Requests"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(420, 360);

    auto *layout = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    rows = new QVBoxLayout(content);
    rows->setAlignment(Qt::AlignTop);
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);

    status = new QLabel(this);
    status->setWordWrap(true);
    status->hide();
    layout->addWidget(status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(instance->relationships(), &Core::RelationshipManager::relationshipChanged, this, &RequestsDialog::rebuild);
    connect(instance, &Core::ClientInstance::messageRequestsChanged, this, &RequestsDialog::rebuild);
    rebuild();
}

int RequestsDialog::pendingCount(Core::ClientInstance *instance)
{
    if (!instance)
        return 0;
    int count = int(instance->messageRequestChannels().size());
    for (const auto &rel : instance->relationships()->pendingRequests())
        if (rel.type.get() == Discord::RelationshipType::INCOMING_REQUEST)
            ++count;
    return count;
}

void RequestsDialog::rebuild()
{
    while (QLayoutItem *item = rows->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (!instance)
        return;

    auto *users = instance->users();
    auto *client = instance->discord();

    QList<Discord::Relationship> incoming, outgoing;
    for (const auto &rel : instance->relationships()->pendingRequests())
        (rel.type.get() == Discord::RelationshipType::INCOMING_REQUEST ? incoming : outgoing).append(rel);

    auto friendRows = [&](const QList<Discord::Relationship> &list, bool isIncoming) {
        for (const auto &rel : list) {
            const Core::Snowflake userId = rel.id.get();
            if (rel.user.hasValue())
                users->saveUser(rel.user.get());
            const QString name = users->getDisplayName(userId);
            const QString detail = rel.isSpamRequest.valueOr(false) ? tr("Possible spam") : QString();
            QList<QPair<QString, std::function<void()>>> actions;
            if (isIncoming) {
                actions.append({ tr("Accept"), [this, client, userId] {
                                    runAction(tr("accept the friend request"), [client, userId](auto done) {
                                        client->acceptFriendRequest(userId, [done](const auto &r) { done(r.success(), r.error); });
                                    });
                                } });
            }
            actions.append({ isIncoming ? tr("Ignore") : tr("Cancel"), [this, client, userId] {
                                runAction(tr("remove the friend request"), [client, userId](auto done) {
                                    client->removeRelationship(userId, [done](const auto &r) { done(r.success(), r.error); });
                                });
                            } });
            addRow(name, detail, actions);
        }
    };

    if (!incoming.isEmpty()) {
        addSection(tr("Incoming friend requests"));
        friendRows(incoming, true);
    }
    if (!outgoing.isEmpty()) {
        addSection(tr("Sent friend requests"));
        friendRows(outgoing, false);
    }

    const QList<Core::Snowflake> requests = instance->messageRequestChannels();
    if (!requests.isEmpty()) {
        addSection(tr("Message requests"));
        for (Core::Snowflake channelId : requests) {
            QString name;
            if (auto channel = instance->getChannel(channelId)) {
                name = channel->name.valueOr();
                if (name.isEmpty() && channel->recipientIds.hasValue() && !channel->recipientIds->isEmpty())
                    name = users->getDisplayName(channel->recipientIds->first());
            }
            if (name.isEmpty())
                name = tr("Unknown user");

            addRow(name, QString(),
                   { { tr("View"), [this, channelId] { if (openChannel) openChannel(channelId); } },
                     { tr("Accept"), [this, client, channelId] {
                          runAction(tr("accept the message request"), [client, channelId](auto done) {
                              client->acceptMessageRequest(channelId, [done](const auto &r) { done(r.success(), r.error); });
                          });
                      } },
                     { tr("Ignore"), [this, client, channelId] {
                          runAction(tr("ignore the message request"), [client, channelId](auto done) {
                              client->rejectMessageRequest(channelId, [done](const auto &r) { done(r.success(), r.error); });
                          });
                      } } });
        }
    }

    if (rows->isEmpty()) {
        auto *empty = new QLabel(tr("No pending requests."));
        empty->setAlignment(Qt::AlignCenter);
        empty->setEnabled(false);
        rows->addWidget(empty);
    }
}

void RequestsDialog::addSection(const QString &title)
{
    auto *label = new QLabel(title.toUpper());
    QFont font = label->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 0.85);
    label->setFont(font);
    label->setContentsMargins(0, rows->isEmpty() ? 0 : 10, 0, 2);
    rows->addWidget(label);
}

void RequestsDialog::addRow(const QString &name, const QString &detail, const QList<QPair<QString, std::function<void()>>> &actions)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 2, 0, 2);
    auto *label = new QLabel(detail.isEmpty() ? name.toHtmlEscaped()
                                              : name.toHtmlEscaped() + QStringLiteral(" <small>(%1)</small>").arg(detail.toHtmlEscaped()));
    label->setTextFormat(Qt::RichText);
    layout->addWidget(label, 1);
    for (const auto &[text, action] : actions) {
        auto *button = new QPushButton(text, row);
        connect(button, &QPushButton::clicked, this, action);
        layout->addWidget(button);
    }
    rows->addWidget(row);
}

void RequestsDialog::runAction(const QString &what, const std::function<void(std::function<void(bool, const QString &)>)> &call)
{
    status->hide();
    QPointer<RequestsDialog> self(this);
    call([self, what](bool ok, const QString &error) {
        if (!self || ok)
            return; // the gateway event that follows success rebuilds the list
        self->status->setText(tr("Couldn't %1: %2").arg(what, error.isEmpty() ? tr("unknown error") : error));
        self->status->show();
    });
}

} // namespace UI
} // namespace Acheron
