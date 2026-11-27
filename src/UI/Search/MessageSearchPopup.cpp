#include "MessageSearchPopup.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/MessageManager.hpp"
#include "Core/UserManager.hpp"
#include "Core/Theme/Manager.hpp"
#include "Discord/Client.hpp"
#include "UI/Chat/MessageTextBrowser.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

namespace {
constexpr int PopupWidth = 520;
constexpr int PopupMargin = 16;
constexpr int CardMargin = 10;
constexpr int CardBorder = 1;

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

// "has:link" style filters, like Discord's search bar; everything else is the text to find
const QStringList HasFilters = { "link", "embed", "file", "image", "video", "sound", "sticker", "poll" };
} // namespace

MessageSearchPopup::MessageSearchPopup(Core::ImageManager *images, QWidget *parent)
    : BasePopup(parent), images(images)
{
    QFrame *c = getContainer();
    c->setFixedWidth(PopupWidth);

    auto *layout = new QVBoxLayout(c);
    layout->setContentsMargins(PopupMargin, PopupMargin, PopupMargin, PopupMargin);
    layout->setSpacing(8);

    auto *bar = new QHBoxLayout;
    input = new QLineEdit(c);
    input->setPlaceholderText(tr("Search (has:link, has:image, has:file...)"));
    input->setClearButtonEnabled(true);
    connect(input, &QLineEdit::returnPressed, this, &MessageSearchPopup::startSearch);
    bar->addWidget(input, 1);
    scope = new QComboBox(c);
    connect(scope, qOverload<int>(&QComboBox::activated), this, [this](int) { // Qt 5 also has a QString overload
        if (!input->text().trimmed().isEmpty())
            startSearch();
    });
    bar->addWidget(scope);
    layout->addLayout(bar);

    scroll = new QScrollArea(c);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scroll->setMinimumHeight(420);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *items = new QWidget(scroll);
    auto *itemsLayout = new QVBoxLayout(items);
    itemsLayout->setContentsMargins(0, 0, 0, 0);
    itemsLayout->setSpacing(8);
    statusLabel = mutedLabel(QString(), items);
    statusLabel->setAlignment(Qt::AlignCenter);
    itemsLayout->addWidget(statusLabel);
    cardsLayout = new QVBoxLayout;
    cardsLayout->setSpacing(8);
    itemsLayout->addLayout(cardsLayout);
    itemsLayout->addStretch(1);
    scroll->setWidget(items);
    layout->addWidget(scroll, 1);

    loadMoreButton = new QPushButton(tr("Load more results"), c);
    loadMoreButton->setCursor(Qt::PointingHandCursor);
    loadMoreButton->hide();
    connect(loadMoreButton, &QPushButton::clicked, this, &MessageSearchPopup::fetchPage);
    layout->addWidget(loadMoreButton);

    retryTimer = new QTimer(this);
    retryTimer->setSingleShot(true);
    connect(retryTimer, &QTimer::timeout, this, &MessageSearchPopup::fetchPage);
}

void MessageSearchPopup::configure(Core::ClientInstance *inst, Core::Snowflake guild, Core::Snowflake channel, const QString &channelName)
{
    const bool samePlace = instance == inst && guildId == guild && channelId == channel;
    instance = inst;
    guildId = guild;
    channelId = channel;

    scope->clear();
    if (guildId.isValid()) {
        scope->addItem(tr("This server"));
        scope->addItem(channelName.isEmpty() ? tr("This channel") : tr("#%1").arg(channelName));
    } else {
        scope->addItem(tr("This conversation"));
    }
    scope->setEnabled(scope->count() > 1);

    if (!samePlace) {
        input->clear();
        clearResults();
        setStatus(QString());
    }
    input->setFocus();
    input->selectAll();
}

void MessageSearchPopup::hideEvent(QHideEvent *event)
{
    retryTimer->stop();
    BasePopup::hideEvent(event);
}

void MessageSearchPopup::startSearch()
{
    QString text = input->text().simplified();
    has.clear();
    static const QRegularExpression hasRe(QStringLiteral(R"((?:^|\s)has:(\w+))"));
    for (auto it = hasRe.globalMatch(text); it.hasNext();) {
        const QString value = it.next().captured(1).toLower();
        if (HasFilters.contains(value) && !has.contains(value))
            has.append(value);
    }
    text.remove(hasRe);
    query = text.trimmed();

    clearResults();
    if (query.isEmpty() && has.isEmpty()) {
        setStatus(QString());
        return;
    }
    fetchPage();
}

void MessageSearchPopup::clearResults()
{
    ++generation; // drop replies to searches already in flight
    retryTimer->stop();
    loading = false;
    offset = 0;
    total = 0;
    loadMoreButton->hide();
    while (QLayoutItem *item = cardsLayout->takeAt(0)) {
        if (QWidget *card = item->widget()) {
            card->hide();
            card->deleteLater();
        }
        delete item;
    }
    scroll->verticalScrollBar()->setValue(0);
}

void MessageSearchPopup::setStatus(const QString &text)
{
    statusLabel->setText(text);
    statusLabel->setVisible(!text.isEmpty());
}

void MessageSearchPopup::fetchPage()
{
    if (!instance || loading)
        return;
    loading = true;
    loadMoreButton->setEnabled(false);
    if (offset == 0)
        setStatus(tr("Searching..."));

    Discord::Client::MessageSearchQuery search;
    search.content = query;
    search.has = has;
    search.guildId = guildId;
    search.channelId = !guildId.isValid() || scope->currentIndex() == 1 ? channelId : Core::Snowflake::Invalid;
    search.offset = offset;

    const QPointer<MessageSearchPopup> self(this);
    const int requestGeneration = generation;
    instance->discord()->searchMessages(search, [self, requestGeneration](const Core::Result<Discord::Client::MessageSearchPage> &result) {
        if (!self || requestGeneration != self->generation)
            return;
        self->loading = false;
        self->loadMoreButton->setEnabled(true);

        if (!result.success()) {
            self->setStatus(tr("Search failed: %1").arg(result.error));
            self->loadMoreButton->setVisible(self->offset > 0);
            return;
        }
        const auto &page = *result.value;
        if (page.indexingRetryMs >= 0) {
            self->setStatus(tr("Discord is still indexing these messages, retrying shortly..."));
            self->retryTimer->start(page.indexingRetryMs);
            return;
        }

        self->total = page.totalResults;
        self->offset += Discord::Client::MessageSearchPageSize;
        for (const auto &msg : page.messages)
            self->addCard(msg);

        self->setStatus(self->total == 0 ? tr("No results.") : tr("%n result(s)", nullptr, self->total));
        // offset is capped by Discord at 9975
        self->loadMoreButton->setVisible(self->offset < self->total && self->offset <= 9975);
    });
}

int MessageSearchPopup::bodyWidth() const
{
    const int scrollBar = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scroll);
    return PopupWidth - 2 * PopupMargin - 2 * (CardMargin + CardBorder) - scrollBar;
}

void MessageSearchPopup::addCard(const Discord::Message &found)
{
    Discord::Message msg = found;
    instance->messages()->prepareForDisplay(msg);

    const Core::Snowflake msgChannelId = msg.channelId.hasValue() ? msg.channelId.get() : channelId;
    const Core::Snowflake messageId = msg.id.get();

    auto *card = new QFrame;
    card->setObjectName("searchCard");
    card->setStyleSheet(QStringLiteral("#searchCard { border: %1px solid rgba(128, 128, 128, 0.25); border-radius: 6px; }").arg(CardBorder));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(CardMargin, 8, CardMargin, 8);
    cardLayout->setSpacing(4);

    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    auto *author = new QLabel(msg.author.hasValue() ? instance->users()->getAuthorDisplayName(msg.author.get(), guildId) : QString(), card);
    author->setTextFormat(Qt::PlainText);
    QFont bold = author->font();
    bold.setBold(true);
    author->setFont(bold);
    header->addWidget(author);

    if (guildId.isValid() && scope->currentIndex() == 0) {
        if (auto ch = instance->getChannel(msgChannelId); ch && ch->name.hasValue())
            header->addWidget(mutedLabel(QStringLiteral("#") + ch->name.get(), card));
    }
    if (msg.timestamp.hasValue())
        header->addWidget(mutedLabel(QLocale().toString(msg.timestamp->toLocalTime(), QLocale::ShortFormat), card));
    header->addStretch(1);

    auto *jump = new QPushButton(tr("Jump"), card);
    jump->setCursor(Qt::PointingHandCursor);
    jump->setFlat(true);
    connect(jump, &QPushButton::clicked, this, [this, msgChannelId, messageId] {
        emit messageActivated(msgChannelId, messageId);
        accept();
    });
    header->addWidget(jump);
    cardLayout->addLayout(header);

    const Discord::Message &shown = msg.contentMessage();
    if (!shown.parsedContentCached.isEmpty()) {
        const int width = bodyWidth();
        auto *body = new MessageTextBrowser(images, instance->accountId(), card);
        body->setFont(Core::Theme::Manager::instance().font(Core::Theme::FontRole::Message));
        body->setFixedWidth(width);
        body->fitHeightToDocument();
        body->setMessageHtml(shown.parsedContentCached, width);
        connect(body, &MessageTextBrowser::linkActivated, this, [this](const QString &url) {
            emit linkActivated(url);
            accept();
        });
        cardLayout->addWidget(body);
    }

    QStringList extras;
    if (shown.attachments.hasValue())
        for (const auto &att : shown.attachments.get())
            extras.append(QStringLiteral("📎 ") + att.filename.valueOr(tr("attachment")));
    if (shown.stickerItems.hasValue() && !shown.stickerItems->isEmpty())
        extras.append(tr("Sticker"));
    if (shown.parsedContentCached.isEmpty() && extras.isEmpty() && shown.embeds.hasValue() && !shown.embeds->isEmpty())
        extras.append(tr("Embed"));
    for (const QString &text : extras)
        cardLayout->addWidget(mutedLabel(text, card));

    cardsLayout->addWidget(card);
}

} // namespace UI
} // namespace Acheron
