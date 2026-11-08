#include "PinnedMessagesPopup.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/MessageManager.hpp"
#include "Core/UserManager.hpp"
#include "Core/Theme/Icons.hpp"
#include "Core/Theme/Manager.hpp"
#include "UI/Chat/MessageTextBrowser.hpp"
#include "UI/Dialogs/ConfirmPopup.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

namespace {
constexpr int PopupWidth = 480;
constexpr int PopupMargin = 16;
constexpr int CardMargin = 10;
constexpr int CardBorder = 1;
constexpr int LoadMoreDistance = 250;

bool shiftHeld()
{
    return QApplication::keyboardModifiers() & Qt::ShiftModifier;
}

QLabel *mutedLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
    label->setPalette(pal);
    return label;
}
} // namespace

PinnedMessagesPopup::PinnedMessagesPopup(Core::ImageManager *images, QWidget *parent)
    : BasePopup(parent), images(images)
{
    QFrame *c = getContainer();
    c->setFixedWidth(PopupWidth);

    auto *layout = new QVBoxLayout(c);
    layout->setContentsMargins(PopupMargin, PopupMargin, PopupMargin, PopupMargin);
    layout->setSpacing(8);

    titleLabel = new QLabel(tr("Pinned Messages"), c);
    titleLabel->setTextFormat(Qt::PlainText);
    {
        QFont f = titleLabel->font();
        f.setBold(true);
        f.setPointSize(f.pointSize() + 2);
        titleLabel->setFont(f);
    }
    layout->addWidget(titleLabel);

    scroll = new QScrollArea(c);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scroll->setMinimumHeight(420);
    scroll->setFrameShape(QFrame::NoFrame);
    connect(scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, &PinnedMessagesPopup::loadMoreIfNearBottom);

    auto *items = new QWidget(scroll);
    auto *itemsLayout = new QVBoxLayout(items);
    itemsLayout->setContentsMargins(0, 0, 0, 0);
    itemsLayout->setSpacing(8);
    cardsLayout = new QVBoxLayout;
    cardsLayout->setSpacing(8);
    itemsLayout->addLayout(cardsLayout);

    statusLabel = new QLabel(items);
    statusLabel->setTextFormat(Qt::PlainText);
    statusLabel->setWordWrap(true);
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->setContentsMargins(0, 24, 0, 8);
    itemsLayout->addWidget(statusLabel);

    tipLabel = mutedLabel(QString(), items);
    tipLabel->setAlignment(Qt::AlignCenter);
    itemsLayout->addWidget(tipLabel);

    itemsLayout->addStretch(1);
    scroll->setWidget(items);
    layout->addWidget(scroll, 1);

    loadMoreButton = new QPushButton(c);
    loadMoreButton->setCursor(Qt::PointingHandCursor);
    loadMoreButton->hide();
    connect(loadMoreButton, &QPushButton::clicked, this, &PinnedMessagesPopup::loadMore);
    layout->addWidget(loadMoreButton, 0);
}

void PinnedMessagesPopup::configure(Core::ClientInstance *inst, Core::Snowflake chId, const QString &channelName, bool unpinAllowed)
{
    instance = inst;
    channelId = chId;
    isPrivate = !inst->discord()->getGuildIdForChannel(chId).isValid();
    canUnpin = unpinAllowed;
    titleLabel->setText(channelName.isEmpty() ? tr("Pinned Messages") : tr("Pinned Messages — #%1").arg(channelName));

    disconnect(pinsConnection);
    disconnect(loadStateConnection);
    pinsConnection = connect(pins(), &Core::ChannelPins::pinsChanged, this, &PinnedMessagesPopup::onPinsChanged);
    loadStateConnection = connect(pins(), &Core::ChannelPins::loadStateChanged, this, &PinnedMessagesPopup::onLoadStateChanged);

    rebuildCards();
    updateStatus();
    scroll->verticalScrollBar()->setValue(0);
    pins()->fetchFirstPage(channelId);
}

void PinnedMessagesPopup::confirmUnpin(QWidget *parent, Core::ChannelPins *pins, Core::Snowflake channelId, Core::Snowflake messageId)
{
    if (shiftHeld()) {
        pins->unpin(channelId, messageId);
        return;
    }

    const QString text = tr("You sure you want to remove this pinned message?") + QStringLiteral("<br><br><small>") +
                         tr("You can hold down shift when clicking <b>unpin message</b> to bypass this confirmation entirely.") + QStringLiteral("</small>");
    auto *confirm = new ConfirmPopup(tr("Unpin Message"), text, tr("Remove it please!"), parent);
    confirm->setAttribute(Qt::WA_DeleteOnClose);
    const QPointer<Core::ChannelPins> target(pins);
    connect(confirm, &QDialog::accepted, pins, [target, channelId, messageId]() {
        if (target)
            target->unpin(channelId, messageId);
    });
    confirm->open();
}

Core::ChannelPins *PinnedMessagesPopup::pins() const
{
    return instance->messages()->pins();
}

const Core::ChannelPins::Channel *PinnedMessagesPopup::knownPins() const
{
    return instance ? pins()->channel(channelId) : nullptr;
}

int PinnedMessagesPopup::bodyWidth() const
{
    const int scrollBar = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scroll);
    return PopupWidth - 2 * PopupMargin - 2 * (CardMargin + CardBorder) - scrollBar;
}

void PinnedMessagesPopup::onPinsChanged(Core::Snowflake changedChannelId)
{
    if (changedChannelId != channelId)
        return;
    rebuildCards();
    updateStatus();
}

void PinnedMessagesPopup::onLoadStateChanged(Core::Snowflake changedChannelId)
{
    if (changedChannelId == channelId)
        updateStatus();
}

void PinnedMessagesPopup::rebuildCards()
{
    while (QLayoutItem *item = cardsLayout->takeAt(0)) {
        if (QWidget *card = item->widget()) {
            card->hide();
            card->deleteLater();
        }
        delete item;
    }

    if (const auto *known = knownPins())
        for (const auto &pin : known->newestFirst)
            addCard(pin);
}

void PinnedMessagesPopup::updateStatus()
{
    using LoadState = Core::ChannelPins::LoadState;
    const auto *known = knownPins();
    const LoadState state = known ? known->state : LoadState::Loading;
    const bool empty = !known || known->newestFirst.isEmpty();

    QString status;
    QString tip;
    if (state == LoadState::Failed)
        status = tr("Failed to load pinned messages.");
    else if (state == LoadState::Loading)
        status = tr("Loading...");
    else if (empty && isPrivate)
        status = tr("This direct message doesn't have any pinned messages... yet.");
    else if (empty)
        status = tr("This channel doesn't have any pinned messages... yet.");

    if (empty && state == LoadState::Finished)
        tip = isPrivate ? tr("You can pin a message from its context menu.") : tr("Users with the 'Pin Messages' permission can pin a message from its context menu.");

    statusLabel->setText(status);
    statusLabel->setVisible(!status.isEmpty());
    tipLabel->setText(tip);
    tipLabel->setVisible(!tip.isEmpty());

    loadMoreButton->setText(state == LoadState::Failed ? tr("Try Again") : tr("Load more messages"));
    loadMoreButton->setVisible(state == LoadState::Failed || (state == LoadState::HasMore && !empty));
}

void PinnedMessagesPopup::loadMore()
{
    if (!instance)
        return;
    const auto *known = knownPins();
    if (!known || known->newestFirst.isEmpty())
        pins()->fetchFirstPage(channelId);
    else
        pins()->fetchPageAfterOldest(channelId);
}

void PinnedMessagesPopup::loadMoreIfNearBottom()
{
    const auto *known = knownPins();
    if (!known || known->state != Core::ChannelPins::LoadState::HasMore)
        return;
    const QScrollBar *bar = scroll->verticalScrollBar();
    if (bar->maximum() - bar->value() < LoadMoreDistance)
        pins()->fetchPageAfterOldest(channelId);
}

void PinnedMessagesPopup::addCard(const Core::ChannelPins::Pinned &pin)
{
    const Discord::Message &msg = pin.message;
    const Core::Snowflake msgChannelId = msg.channelId.hasValue() ? msg.channelId.get() : channelId;
    const Core::Snowflake messageId = msg.id.get();
    const Core::Snowflake guildId = instance->discord()->getGuildIdForChannel(msgChannelId);

    auto *card = new QFrame;
    card->setObjectName("pinCard");
    card->setStyleSheet(QStringLiteral("#pinCard { border: %1px solid rgba(128, 128, 128, 0.25); border-radius: 6px; }").arg(CardBorder));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(CardMargin, 8, CardMargin, 8);
    cardLayout->setSpacing(4);

    auto *header = new QHBoxLayout;
    header->setSpacing(8);

    auto *author = new QLabel(msg.author.hasValue() ? instance->users()->getAuthorDisplayName(msg.author.get(), guildId) : QString(), card);
    author->setTextFormat(Qt::PlainText);
    {
        QFont f = author->font();
        f.setBold(true);
        author->setFont(f);
    }
    header->addWidget(author);

    if (msg.timestamp.hasValue())
        header->addWidget(mutedLabel(QLocale().toString(msg.timestamp->toLocalTime(), QLocale::ShortFormat), card));
    header->addStretch(1);

    auto *jump = new QPushButton(tr("Jump"), card);
    jump->setCursor(Qt::PointingHandCursor);
    jump->setFlat(true);
    connect(jump, &QPushButton::clicked, this, [this, msgChannelId, messageId]() {
        emit messageActivated(msgChannelId, messageId);
        if (!shiftHeld())
            accept();
    });
    header->addWidget(jump);

    if (canUnpin) {
        auto *unpin = new QToolButton(card);
        unpin->setIcon(Core::Theme::Icons::icon(Core::Theme::Icons::Name::X, Core::Theme::Token::PrimaryText));
        unpin->setToolTip(tr("Unpin Message"));
        unpin->setAutoRaise(true);
        unpin->setCursor(Qt::PointingHandCursor);
        connect(unpin, &QToolButton::clicked, this, [this, msgChannelId, messageId]() {
            if (instance)
                confirmUnpin(this, pins(), msgChannelId, messageId);
        });
        header->addWidget(unpin);
    }
    cardLayout->addLayout(header);

    if (!msg.parsedContentCached.isEmpty()) {
        const int width = bodyWidth();
        auto *body = new MessageTextBrowser(images, instance->accountId(), card);
        body->setFont(Core::Theme::Manager::instance().font(Core::Theme::FontRole::Message));
        body->setFixedWidth(width);
        body->fitHeightToDocument();
        body->setMessageHtml(msg.parsedContentCached, width);
        connect(body, &MessageTextBrowser::linkActivated, this, [this](const QString &url) {
            emit linkActivated(url);
            accept();
        });
        cardLayout->addWidget(body);
    }

    // attachments, embeds and stickers are not rendered here, so name them to keep the card from looking empty
    QStringList extras;
    if (msg.attachments.hasValue())
        for (const auto &att : msg.attachments.get())
            extras.append(QStringLiteral("📎 ") + att.filename.valueOr(tr("attachment")));
    if (msg.stickerItems.hasValue() && !msg.stickerItems->isEmpty())
        extras.append(tr("Sticker"));
    if (msg.parsedContentCached.isEmpty() && extras.isEmpty() && msg.embeds.hasValue() && !msg.embeds->isEmpty())
        extras.append(tr("Embed"));

    for (const QString &text : extras) {
        auto *label = new QLabel(text, card);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        cardLayout->addWidget(label);
    }

    cardsLayout->addWidget(card);
}

} // namespace UI
} // namespace Acheron
