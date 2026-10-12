#include "PinnedMessagesPopup.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/MessageManager.hpp"
#include "Core/UserManager.hpp"
#include "Core/Theme/Manager.hpp"
#include "UI/Chat/MessageTextBrowser.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

namespace {
constexpr int PopupWidth = 480;
constexpr int BodyWidth = PopupWidth - 2 * 16 - 2 * 10 - 12; // popup and card margins, scrollbar
} // namespace

PinnedMessagesPopup::PinnedMessagesPopup(Core::ImageManager *images, QWidget *parent)
    : BasePopup(parent), images(images)
{
    QFrame *c = getContainer();
    c->setFixedWidth(PopupWidth);

    auto *layout = new QVBoxLayout(c);
    layout->setContentsMargins(16, 16, 16, 16);
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

    auto *scroll = new QScrollArea(c);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumHeight(420);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *items = new QWidget(scroll);
    itemsLayout = new QVBoxLayout(items);
    itemsLayout->setContentsMargins(0, 0, 0, 0);
    itemsLayout->setSpacing(8);
    itemsLayout->addStretch(1);
    scroll->setWidget(items);
    layout->addWidget(scroll, 1);

    loadMoreButton = new QPushButton(tr("Load more"), c);
    loadMoreButton->setCursor(Qt::PointingHandCursor);
    loadMoreButton->hide();
    connect(loadMoreButton, &QPushButton::clicked, this, &PinnedMessagesPopup::loadPage);
    layout->addWidget(loadMoreButton, 0);
}

void PinnedMessagesPopup::configure(Core::ClientInstance *inst, Core::Snowflake chId,
                                    const QString &channelName)
{
    instance = inst;
    channelId = chId;
    titleLabel->setText(channelName.isEmpty() ? tr("Pinned Messages")
                                              : tr("Pinned Messages — #%1").arg(channelName));
    reload();
}

void PinnedMessagesPopup::reload()
{
    ++loadGeneration;
    clearItems();
    oldestPinnedAt = QDateTime();
    loadMoreButton->hide();
    loadPage();
}

void PinnedMessagesPopup::clearItems()
{
    // the trailing stretch stays
    while (itemsLayout->count() > 1) {
        QLayoutItem *item = itemsLayout->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

void PinnedMessagesPopup::addNotice(const QString &text)
{
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setContentsMargins(0, 24, 0, 24);
    itemsLayout->insertWidget(itemsLayout->count() - 1, label);
}

void PinnedMessagesPopup::addPin(const Discord::Client::MessagePin &pin)
{
    const Discord::Message &msg = pin.message;
    const Core::Snowflake msgChannelId = msg.channelId.hasValue() ? msg.channelId.get() : channelId;
    const Core::Snowflake messageId = msg.id.get();
    const Core::Snowflake guildId = instance->discord()->getGuildIdForChannel(msgChannelId);

    auto *card = new QFrame;
    card->setObjectName("pinCard");
    card->setStyleSheet("#pinCard { border: 1px solid rgba(128, 128, 128, 0.25); border-radius: 6px; }");
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(10, 8, 10, 8);
    cardLayout->setSpacing(4);

    auto *header = new QHBoxLayout;
    header->setSpacing(8);

    auto *author = new QLabel(msg.author.hasValue() ? instance->users()->getAuthorDisplayName(msg.author.get(), guildId)
                                                    : QString(),
                              card);
    author->setTextFormat(Qt::PlainText);
    {
        QFont f = author->font();
        f.setBold(true);
        author->setFont(f);
    }
    header->addWidget(author);

    if (msg.timestamp.hasValue()) {
        auto *date = new QLabel(QLocale().toString(msg.timestamp->toLocalTime(), QLocale::ShortFormat), card);
        date->setTextFormat(Qt::PlainText);
        QPalette pal = date->palette();
        pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
        date->setPalette(pal);
        header->addWidget(date);
    }
    header->addStretch(1);

    auto *jump = new QPushButton(tr("Jump"), card);
    jump->setCursor(Qt::PointingHandCursor);
    jump->setFlat(true);
    connect(jump, &QPushButton::clicked, this, [this, msgChannelId, messageId]() {
        emit messageActivated(msgChannelId, messageId);
        accept();
    });
    header->addWidget(jump);
    cardLayout->addLayout(header);

    const QString content = msg.content.valueOr(QString());
    if (!content.isEmpty()) {
        auto *body = new MessageTextBrowser(images, instance->accountId(), card);
        body->setFont(Core::Theme::Manager::instance().font(Core::Theme::FontRole::Message));
        body->setFixedWidth(BodyWidth);
        body->fitHeightToDocument();
        body->setMessageHtml(instance->messages()->messageContentHtml(content, msgChannelId), BodyWidth);
        connect(body, &MessageTextBrowser::linkActivated, this, [this](const QString &url) {
            emit linkActivated(url);
            accept();
        });
        cardLayout->addWidget(body);
    }

    // attachments and stickers are not rendered, name them so the card is not blank
    QStringList extras;
    if (msg.attachments.hasValue())
        for (const auto &att : msg.attachments.get())
            extras.append(QStringLiteral("📎 ") + att.filename.valueOr(tr("attachment")));
    if (msg.stickerItems.hasValue() && !msg.stickerItems->isEmpty())
        extras.append(tr("Sticker"));
    if (content.isEmpty() && extras.isEmpty() && msg.embeds.hasValue() && !msg.embeds->isEmpty())
        extras.append(tr("Embed"));

    for (const QString &text : extras) {
        auto *label = new QLabel(text, card);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        cardLayout->addWidget(label);
    }

    itemsLayout->insertWidget(itemsLayout->count() - 1, card);
}

void PinnedMessagesPopup::loadPage()
{
    if (!instance)
        return;

    loadMoreButton->setEnabled(false);

    QPointer<PinnedMessagesPopup> self(this);
    int gen = loadGeneration;
    instance->discord()->fetchMessagePins(
            channelId, oldestPinnedAt,
            [this, self, gen](const Core::Result<Discord::Client::MessagePinsResult> &res) {
                if (!self || gen != loadGeneration || !instance)
                    return;
                loadMoreButton->setEnabled(true);
                if (!res.success()) {
                    addNotice(tr("Failed to load pinned messages"));
                    loadMoreButton->hide();
                    return;
                }

                const auto &page = *res.value;
                if (page.items.isEmpty() && !oldestPinnedAt.isValid())
                    addNotice(tr("This channel doesn't have any pinned messages yet."));

                for (const auto &pin : page.items) {
                    addPin(pin);
                    if (!oldestPinnedAt.isValid() || pin.pinnedAt < oldestPinnedAt)
                        oldestPinnedAt = pin.pinnedAt;
                }
                loadMoreButton->setVisible(page.hasMore && oldestPinnedAt.isValid());
            });
}

} // namespace UI
} // namespace Acheron
