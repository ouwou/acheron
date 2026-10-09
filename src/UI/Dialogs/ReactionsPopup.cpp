#include "ReactionsPopup.hpp"

#include <QAbstractListModel>
#include <QCursor>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>

#include "Core/ClientInstance.hpp"
#include "Core/Theme/Icons.hpp"
#include "UI/Chat/ReactionTooltip.hpp"
#include "UI/Emoji/EmojiGlyphs.hpp"
#include "UI/Emoji/EmojiPainting.hpp"
#include "UI/Emoji/PaintedImages.hpp"

namespace Acheron {
namespace UI {

class ReactorListModel : public QAbstractListModel
{
public:
    enum Role {
        UserIdRole = Qt::UserRole + 1,
        UsernameRole,
        AvatarUrlRole,
        RemovableRole,
    };

    struct Reactor
    {
        Core::Snowflake userId;
        QString displayName;
        QString username;
        QUrl avatarUrl;
        bool removable = false;
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(reactors.size()); }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= reactors.size())
            return {};

        const Reactor &reactor = reactors[index.row()];
        switch (role) {
        case Qt::DisplayRole:
            return reactor.displayName;
        case UserIdRole:
            return quint64(reactor.userId);
        case UsernameRole:
            return reactor.username;
        case AvatarUrlRole:
            return reactor.avatarUrl;
        case RemovableRole:
            return reactor.removable;
        }
        return {};
    }

    void clear()
    {
        beginResetModel();
        reactors.clear();
        endResetModel();
    }

    void showWithoutResetting(const QList<Reactor> &current)
    {
        QSet<Core::Snowflake> currentIds;
        for (const Reactor &reactor : current)
            currentIds.insert(reactor.userId);

        QSet<Core::Snowflake> keptIds;
        for (int row = int(reactors.size()) - 1; row >= 0; --row) {
            if (currentIds.contains(reactors[row].userId)) {
                keptIds.insert(reactors[row].userId);
                continue;
            }
            beginRemoveRows({}, row, row);
            reactors.removeAt(row);
            endRemoveRows();
        }

        QList<Reactor> arrived;
        for (const Reactor &reactor : current) {
            if (!keptIds.contains(reactor.userId))
                arrived.append(reactor);
        }
        if (arrived.isEmpty())
            return;

        beginInsertRows({}, int(reactors.size()), int(reactors.size() + arrived.size()) - 1);
        reactors.append(arrived);
        endInsertRows();
    }

private:
    QList<Reactor> reactors;
};

class ReactorDelegate : public QStyledItemDelegate
{
public:
    static constexpr int RowHeight = 34;
    static constexpr int AvatarPx = 24;
    static constexpr int Padding = 8;
    static constexpr int RemoveButtonPx = 20;
    static constexpr int RemoveIconPx = 14;

    ReactorDelegate(PaintedImages *avatars, QObject *parent) : QStyledItemDelegate(parent), avatars(avatars) {}

    [[nodiscard]] static QRect removeButtonRect(const QRect &row)
    {
        return QRect(row.right() - Padding - RemoveButtonPx + 1, row.top() + (row.height() - RemoveButtonPx) / 2, RemoveButtonPx, RemoveButtonPx);
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(option.rect.width(), RowHeight);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

        const QRect row = option.rect;
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        if (hovered) {
            QColor hoverColor = option.palette.highlight().color();
            hoverColor.setAlpha(30);
            painter->fillRect(row, hoverColor);
        }

        const QRect avatarRect(row.left() + Padding, row.top() + (row.height() - AvatarPx) / 2, AvatarPx, AvatarPx);
        QPainterPath avatarShape;
        avatarShape.addEllipse(avatarRect);
        const QPixmap avatar = avatars->pixmap(index.data(ReactorListModel::AvatarUrlRole).toUrl(), AvatarPx, row);
        if (avatar.isNull()) {
            QColor placeholder = option.palette.mid().color();
            placeholder.setAlpha(100);
            painter->fillPath(avatarShape, placeholder);
        } else {
            painter->setClipPath(avatarShape);
            painter->drawPixmap(avatarRect, avatar);
            painter->setClipping(false);
        }

        const bool showRemove = hovered && index.data(ReactorListModel::RemovableRole).toBool();
        const QRect removeRect = removeButtonRect(row);
        if (showRemove) {
            const QColor iconColor = option.palette.color(QPalette::Text);
            const QPixmap icon = Core::Theme::Icons::pixmap(Core::Theme::Icons::Name::X, RemoveIconPx, iconColor, painter->device()->devicePixelRatioF());
            drawCentered(*painter, removeRect, icon);
        }

        const int textLeft = avatarRect.right() + 1 + Padding;
        const int textRight = showRemove ? removeRect.left() - Padding : row.right() - Padding;
        QRect textRect(textLeft, row.top(), qMax(0, textRight - textLeft), row.height());

        QFont nameFont = option.font;
        nameFont.setWeight(QFont::Medium);
        const QFontMetrics nameMetrics(nameFont);
        const QString name = nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRect.width());
        painter->setFont(nameFont);
        painter->setPen(option.palette.color(QPalette::Text));
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, name);

        textRect.setLeft(textRect.left() + nameMetrics.horizontalAdvance(name) + Padding);
        if (textRect.width() > 0) {
            const QFontMetrics usernameMetrics(option.font);
            painter->setFont(option.font);
            painter->setPen(option.palette.color(QPalette::Disabled, QPalette::Text));
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, usernameMetrics.elidedText(index.data(ReactorListModel::UsernameRole).toString(), Qt::ElideRight, textRect.width()));
        }

        painter->restore();
    }

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        const QRect removeRect = removeButtonRect(option.rect);
        if (event->type() != QEvent::ToolTip || !index.data(ReactorListModel::RemovableRole).toBool() || !removeRect.contains(event->pos()))
            return QStyledItemDelegate::helpEvent(event, view, option, index);

        QToolTip::showText(event->globalPos(), ReactionsPopup::tr("Remove Reaction"), view->viewport(), removeRect);
        return true;
    }

private:
    PaintedImages *avatars;
};

static constexpr int TabIconPx = 20;
static constexpr int AvatarCdnPx = 64;

ReactionsPopup::ReactionsPopup(Core::ImageManager *images, Core::ClientInstance *instance, Core::Snowflake channelId, Core::Snowflake guildId, Core::Snowflake messageId, const std::optional<Core::ReactionRef> &selected, RemovableReactors removable, QWidget *parent)
    : BasePopup(parent), instance(instance), channelId(channelId), guildId(guildId), messageId(messageId), removable(removable), selected(selected)
{
    setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(getContainer());
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *title = new QLabel(tr("Reactions"), getContainer());
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *lists = new QHBoxLayout;
    lists->setSpacing(8);
    layout->addLayout(lists);

    tabList = new QListWidget(getContainer());
    tabList->setFixedSize(92, 380);
    tabList->setIconSize(QSize(TabIconPx, TabIconPx));
    tabList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tabList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    lists->addWidget(tabList);

    reactorView = new QListView(getContainer());
    reactorView->setMinimumWidth(340);
    reactorView->setUniformItemSizes(true);
    reactorView->setSelectionMode(QAbstractItemView::NoSelection);
    reactorView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    reactorView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    reactorView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    reactorView->setMouseTracking(true);
    reactorView->viewport()->setAttribute(Qt::WA_Hover);
    retryPageButton = new QPushButton(tr("Could not load reactions. Retry"), getContainer());
    retryPageButton->hide();

    auto *reactorColumn = new QVBoxLayout;
    reactorColumn->setSpacing(6);
    reactorColumn->addWidget(reactorView, 1);
    reactorColumn->addWidget(retryPageButton);
    lists->addLayout(reactorColumn, 1);

    tabIcons = new PaintedImages(tabList->viewport());
    tabIcons->setSource(images, instance->accountId());
    auto *avatars = new PaintedImages(reactorView->viewport());
    avatars->setSource(images, instance->accountId());

    reactorModel = new ReactorListModel(this);
    reactorView->setModel(reactorModel);
    reactorView->setItemDelegate(new ReactorDelegate(avatars, reactorView));

    connect(tabIcons, &PaintedImages::fetchFinished, this, &ReactionsPopup::refreshTabIcons);
    connect(tabList, &QListWidget::currentRowChanged, this, &ReactionsPopup::onTabSelected);
    connect(reactorView, &QListView::clicked, this, &ReactionsPopup::onReactorClicked);
    connect(reactorView->verticalScrollBar(), &QScrollBar::valueChanged, this, &ReactionsPopup::onReactorsScrolled);
    connect(retryPageButton, &QPushButton::clicked, this, &ReactionsPopup::loadPage);

    auto *messages = instance->messages();
    connect(messages, &Core::MessageManager::reactionsChanged, this, &ReactionsPopup::onReactionsChanged);
    connect(messages, &Core::MessageManager::messageDeleted, this, &ReactionsPopup::onMessageDeleted);
    connect(messages->reactors(), &Core::MessageReactors::reactorsChanged, this, &ReactionsPopup::onReactorsChanged);
    connect(instance, &QObject::destroyed, this, &QDialog::close);

    syncWithMessage();
}

void ReactionsPopup::syncWithMessage()
{
    tabs.clear();
    for (const Discord::Reaction &reaction : instance->messages()->reactionsOf(messageId)) {
        const ReactionState wholeReaction{ reaction.normalCount(), reaction.superCount(), reaction.me.get(), reaction.meBurst.valueOr(false) };
        if (reaction.superCount() > 0)
            tabs.append({ { messageId, reaction.emoji, true }, reaction.superCount(), reaction.getBrightestBurstColor(), wholeReaction });
        if (reaction.normalCount() > 0)
            tabs.append({ { messageId, reaction.emoji, false }, reaction.normalCount(), {}, wholeReaction });
    }
    std::stable_sort(tabs.begin(), tabs.end(), [](const Tab &a, const Tab &b) { return a.count > b.count; });

    if (tabs.isEmpty()) {
        QTimer::singleShot(0, this, &QDialog::close);
        return;
    }
    if (tabRowOf(selected) < 0)
        selected = tabs.first().reaction;

    {
        const QSignalBlocker updatingTabs(tabList);
        while (tabList->count() > tabs.size())
            delete tabList->takeItem(tabList->count() - 1);
        for (int row = 0; row < tabs.size(); ++row) {
            const Tab &tab = tabs[row];
            QListWidgetItem *item = row < tabList->count() ? tabList->item(row) : new QListWidgetItem(tabList);
            item->setIcon(tabIcon(tab.reaction.emoji));
            item->setText(QString::number(tab.count));
            item->setToolTip(reactionEmojiName(tab.reaction.emoji));
            item->setForeground(tab.superTint.isValid() ? QBrush(tab.superTint) : QBrush());
        }
        tabList->setCurrentRow(tabRowOf(selected));
    }

    showSelected();
}

int ReactionsPopup::tabRowOf(const std::optional<Core::ReactionRef> &reaction) const
{
    const auto tab = std::find_if(tabs.cbegin(), tabs.cend(), [&reaction](const Tab &tab) { return reaction && tab.reaction.sameReaction(*reaction); });
    return tab == tabs.cend() ? -1 : int(tab - tabs.cbegin());
}

void ReactionsPopup::showSelected()
{
    const ReactionState state = tabs[tabRowOf(selected)].wholeReaction;
    const bool sameReactionAsShown = shown && shown->sameReaction(*selected);
    if (sameReactionAsShown && state == shownState)
        return;

    shownState = state;
    if (!sameReactionAsShown) {
        shown = selected;
        reactorModel->clear();
        showKnownReactors();
    }
    restartPaging();
}

void ReactionsPopup::showKnownReactors()
{
    QList<ReactorListModel::Reactor> reactors;
    for (const Discord::User &user : instance->messages()->reactors()->reactors(channelId, *shown, Core::MessageReactors::PageSize))
        reactors.append({ user.id.get(), instance->users()->getAuthorDisplayName(user, guildId), user.username.get(), instance->users()->getAvatarUrl(user, guildId, AvatarCdnPx), canRemove(user.id.get()) });
    reactorModel->showWithoutResetting(reactors);
}

void ReactionsPopup::restartPaging()
{
    lastPageEnd.reset();
    awaitingPage = false;
    loadPage();
}

void ReactionsPopup::loadPage()
{
    awaitingPage = true;
    retryPageButton->hide();
    const int request = ++latestPageRequest;
    const QPointer<ReactionsPopup> self(this);
    instance->messages()->reactors()->fetchPage(channelId, *shown, lastPageEnd, [self, request](const Core::Result<QList<Core::Snowflake>> &pageReactorIds) {
        if (!self || request != self->latestPageRequest)
            return;

        self->retryPageButton->setVisible(!pageReactorIds.success());
        if (!pageReactorIds.success())
            return;
        self->awaitingPage = false;
        self->lastPageEnd = pageReactorIds.value->isEmpty() ? std::nullopt : std::optional(pageReactorIds.value->last());
    });
}

bool ReactionsPopup::hasMoreReactors() const
{
    const int shownRow = tabRowOf(shown);
    return shownRow >= 0 && tabs[shownRow].count > reactorModel->rowCount();
}

bool ReactionsPopup::canRemove(Core::Snowflake reactorId) const
{
    switch (removable) {
    case RemovableReactors::None:
        return false;
    case RemovableReactors::OwnOnly:
        return reactorId == instance->accountId();
    case RemovableReactors::Anyone:
        return true;
    }
    return false;
}

QIcon ReactionsPopup::tabIcon(const Discord::Emoji &emoji)
{
    const qreal devicePixelRatio = devicePixelRatioF();
    if (emoji.isUnicode())
        return QIcon(EmojiGlyphs::pixmap(emoji.name.get(), TabIconPx, devicePixelRatio));
    return QIcon(tabIcons->pixmap(emojiStillUrl(emoji.id.get(), devicePixelRatio), TabIconPx, tabList->viewport()->rect()));
}

void ReactionsPopup::refreshTabIcons()
{
    for (int row = 0; row < tabs.size() && row < tabList->count(); ++row)
        tabList->item(row)->setIcon(tabIcon(tabs[row].reaction.emoji));
}

void ReactionsPopup::onReactionsChanged(Core::Snowflake changedMessageId)
{
    if (changedMessageId == messageId)
        syncWithMessage();
}

void ReactionsPopup::onReactorsChanged(Core::Snowflake changedMessageId)
{
    if (changedMessageId == messageId && shown)
        showKnownReactors();
}

void ReactionsPopup::onMessageDeleted(Core::Snowflake, Core::Snowflake deletedMessageId)
{
    if (deletedMessageId == messageId)
        close();
}

void ReactionsPopup::onTabSelected(int row)
{
    if (row < 0 || row >= tabs.size())
        return;
    selected = tabs[row].reaction;
    showSelected();
}

void ReactionsPopup::onReactorClicked(const QModelIndex &index)
{
    const Core::Snowflake reactorId = index.data(ReactorListModel::UserIdRole).toULongLong();
    const QPoint cursor = reactorView->viewport()->mapFromGlobal(QCursor::pos());
    const bool onRemoveButton = ReactorDelegate::removeButtonRect(reactorView->visualRect(index)).contains(cursor);
    if (onRemoveButton && index.data(ReactorListModel::RemovableRole).toBool())
        instance->messages()->removeReactionOf(reactorId, channelId, *shown);
    else
        emit userActivated(reactorId);
}

void ReactionsPopup::onReactorsScrolled()
{
    const QScrollBar *bar = reactorView->verticalScrollBar();
    const bool nearEnd = bar->maximum() - bar->value() <= ReactorDelegate::RowHeight;
    if (nearEnd && hasMoreReactors() && !awaitingPage)
        loadPage();
}

} // namespace UI
} // namespace Acheron
