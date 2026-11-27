#include "RequestsPage.hpp"

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

RequestsPage::RequestsPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 16);

    auto *title = new QLabel(tr("Requests"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.3);
    title->setFont(titleFont);
    layout->addWidget(title);

    status = new QLabel(this);
    status->setWordWrap(true);
    status->hide();
    layout->addWidget(status);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // rows fit the width; long names are elided
    auto *content = new QWidget(scroll);
    content->setMaximumWidth(640); // keep the buttons near the names on wide windows
    rows = new QVBoxLayout(content);
    rows->setAlignment(Qt::AlignTop);
    rows->setContentsMargins(0, 8, 0, 0);
    scroll->setWidget(content);
    scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(scroll, 1);
}

void RequestsPage::setInstance(Core::ClientInstance *newInstance)
{
    for (const auto &c : connections)
        disconnect(c);
    connections.clear();
    instance = newInstance;
    previews.clear();
    status->hide();
    if (instance) {
        connections.append(connect(instance->relationships(), &Core::RelationshipManager::relationshipChanged, this, [this] {
            if (isVisible())
                rebuild();
        }));
        connections.append(connect(instance, &Core::ClientInstance::messageRequestsChanged, this, [this] {
            if (isVisible())
                rebuild();
        }));
    }
    if (isVisible())
        rebuild();
}

void RequestsPage::fetchPreviews(const QList<Core::Snowflake> &missing)
{
    if (fetchingPreviews || !instance)
        return;
    const QList<Core::Snowflake> channelIds = missing.mid(0, 25); // the endpoint's limit; the rest follow after

    fetchingPreviews = true;
    const QPointer<RequestsPage> self(this);
    const QPointer<Core::ClientInstance> forInstance(instance);
    instance->discord()->fetchMessageRequestPreviews(channelIds, [self, forInstance, channelIds](const auto &result) {
        if (!self)
            return;
        self->fetchingPreviews = false;
        if (self->instance != forInstance)
            return;
        for (Core::Snowflake channelId : channelIds) {
            QString text;
            if (result.success() && result.value->contains(channelId)) {
                const Discord::Message &msg = result.value->value(channelId);
                text = msg.content.hasValue() ? msg.content.get().simplified() : QString();
                if (text.isEmpty() && msg.attachments.hasValue() && !msg.attachments->isEmpty())
                    text = tr("Sent an attachment");
            }
            self->previews.insert(channelId, text.isEmpty() ? QString() : text.left(200));
        }
        if (self->isVisible())
            self->rebuild();
    });
}

void RequestsPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    rebuild();
}

void RequestsPage::rebuild()
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
    QList<Core::Snowflake> missing;
    for (Core::Snowflake channelId : requests)
        if (!previews.contains(channelId))
            missing.append(channelId);
    if (!missing.isEmpty())
        fetchPreviews(missing);
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

            const auto preview = previews.constFind(channelId);
            const QString detail = preview != previews.cend() ? *preview : tr("Loading message...");
            addRow(name, detail,
                   { { tr("View"), [this, channelId] { emit openChannelRequested(channelId); } },
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


void RequestsPage::addSection(const QString &title)
{
    auto *label = new QLabel(title.toUpper());
    QFont font = label->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 0.85);
    label->setFont(font);
    label->setContentsMargins(0, rows->isEmpty() ? 0 : 10, 0, 2);
    rows->addWidget(label);
}

void RequestsPage::addRow(const QString &name, const QString &detail, const QList<QPair<QString, std::function<void()>>> &actions)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 2, 0, 2);
    constexpr int NameWidth = 180;
    const QString shown = fontMetrics().elidedText(name, Qt::ElideRight, NameWidth);
    auto *label = new QLabel(detail.isEmpty() ? shown.toHtmlEscaped()
                                              : shown.toHtmlEscaped() + QStringLiteral("<br><small>%1</small>").arg(detail.toHtmlEscaped()));
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setToolTip(name);
    layout->addWidget(label, 1);
    for (const auto &[text, action] : actions) {
        auto *button = new QPushButton(text, row);
        if (text == tr("View"))
            button->setToolTip(tr("Open the conversation in a new tab"));
        connect(button, &QPushButton::clicked, this, action);
        layout->addWidget(button);
    }
    rows->addWidget(row);
}

void RequestsPage::runAction(const QString &what, const std::function<void(std::function<void(bool, const QString &)>)> &call)
{
    status->hide();
    QPointer<RequestsPage> self(this);
    call([self, what](bool ok, const QString &error) {
        if (!self || ok)
            return; // the gateway event that follows success rebuilds the list
        self->status->setText(tr("Couldn't %1: %2").arg(what, error.isEmpty() ? tr("unknown error") : error));
        self->status->show();
    });
}

} // namespace UI
} // namespace Acheron
