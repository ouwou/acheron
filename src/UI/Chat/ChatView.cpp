#include "ChatView.hpp"

#include <QMenu>
#include <QTextDocument>
#include <QTextCursor>
#include <QToolTip>

#include <algorithm>

#include "Core/ImageManager.hpp"
#include "Core/Theme/Icons.hpp"
#include "Core/Theme/Manager.hpp"
#include "Core/TimeUtils.hpp"
#include "Discord/ChannelLink.hpp"
#include "UI/Chat/FrameAnimator.hpp"
#include "UI/Chat/InlineVideoController.hpp"
#include "UI/Chat/MediaTarget.hpp"
#include "UI/Dialogs/ConfirmPopup.hpp"
#include "UI/Emoji/EmojiGlyphs.hpp"
#include "UI/Emoji/EmojiPainting.hpp"
#include "UI/ImageViewer.hpp"
#include "UI/Input/TextEdgeNavigation.hpp"

namespace Acheron {
namespace UI {

struct MediaHit
{
    QUrl imageUrl;
    QPixmap preview;
    bool spoilerHidden = false;
    Snowflake attachmentId;

    QUrl fileUrl;
    QString filename;
    qint64 fileSizeBytes = -1;

    QUrl linkUrl;

    [[nodiscard]] bool isImage() const { return !imageUrl.isEmpty(); }
    [[nodiscard]] bool isFile() const { return !fileUrl.isEmpty(); }
    [[nodiscard]] bool hasLink() const { return !linkUrl.isEmpty() && !linkUrl.isLocalFile(); }
};

static MediaHit mediaAt(const ChatLayout::ResolvedLayout &resolved, const ChatLayout::HitRegion &region,
                        const ChatModel &chatModel)
{
    using Kind = ChatLayout::HitRegion::Kind;
    MediaHit hit;
    auto embedImage = [&hit](const QUrl &proxyUrl, const QUrl &originalUrl, const QPixmap &pixmap) {
        hit.imageUrl = proxyUrl;
        hit.preview = pixmap;
        hit.fileUrl = proxyUrl;
        hit.filename = QFileInfo(proxyUrl.path()).fileName();
        hit.linkUrl = originalUrl;
    };

    switch (region.kind) {
    case Kind::AttachmentImage:
    case Kind::AttachmentVideo:
    case Kind::AttachmentAudio:
    case Kind::AttachmentFile: {
        if (region.index < 0 || region.index >= resolved.ctx.attachments.size())
            break;
        const AttachmentData &att = resolved.ctx.attachments[region.index];
        if (att.isImage) {
            hit.imageUrl = att.proxyUrl;
            hit.preview = att.pixmap;
            hit.spoilerHidden = att.isSpoiler && !chatModel.isSpoilerRevealed(att.id);
        }
        hit.attachmentId = att.id;
        hit.fileUrl = att.originalUrl;
        hit.filename = att.filename;
        hit.fileSizeBytes = att.fileSizeBytes;
        hit.linkUrl = att.originalUrl;
        break;
    }
    case Kind::EmbedThumbnail: {
        if (region.index < 0 || region.index >= resolved.ctx.embeds.size())
            break;
        const EmbedData &embed = resolved.ctx.embeds[region.index];
        if (!embed.thumbnail.isNull())
            embedImage(embed.thumbnailUrl, embed.thumbnailOriginalUrl, embed.thumbnail);
        break;
    }
    case Kind::EmbedImage: {
        if (region.index < 0 || region.index >= resolved.ctx.embeds.size())
            break;
        const EmbedData &embed = resolved.ctx.embeds[region.index];
        if (region.subIndex >= 0 && region.subIndex < embed.images.size()) {
            const EmbedImageData &image = embed.images[region.subIndex];
            embedImage(image.url, image.originalUrl, image.pixmap);
        }
        break;
    }
    case Kind::Sticker: {
        if (region.index < 0 || region.index >= resolved.ctx.stickers.size())
            break;
        const StickerData &sticker = resolved.ctx.stickers[region.index];
        hit.linkUrl = sticker.isAnimated() ? sticker.animatedUrl : sticker.stillUrl;
        break;
    }
    default:
        break;
    }
    return hit;
}

static constexpr int LoadMoreThreshold = 200;

ChatView::ChatView(QWidget *parent) : QListView(parent), hoveredRow(-1), hoveredChar(-1)
{
    setMouseTracking(true);
    setSelectionMode(QAbstractItemView::NoSelection);
    setUniformItemSizes(false);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    verticalScrollBar()->setSingleStep(10);
    setAutoScroll(false);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);

    inlineEditWidget = new QTextEdit(viewport());
    inlineEditWidget->setVisible(false);
    inlineEditWidget->setFrameStyle(QFrame::Box);
    inlineEditWidget->setLineWidth(2);
    inlineEditWidget->installEventFilter(this);

    video = new InlineVideoController(this);
    animator = new FrameAnimator(this);

    jumpToPresentBar = new JumpToPresentBar(this);
    jumpToPresentBar->setVisible(false);
    connect(jumpToPresentBar, &JumpToPresentBar::clicked, this, &ChatView::jumpToPresent);

    actionBar = new MessageActionBar(viewport());
    connect(actionBar, &MessageActionBar::shiftHeldChanged, this, &ChatView::updateHoveredMessage);
    connect(actionBar, &MessageActionBar::triggered, this, &ChatView::onActionBarTriggered);
    connect(actionBar, &MessageActionBar::moreRequested, this, &ChatView::onActionBarMoreRequested);
    connect(actionBar, &MessageActionBar::quickReactionTriggered, this, &ChatView::onActionBarQuickReaction);
    connect(actionBar, &MessageActionBar::reactionPickerRequested, this, &ChatView::onActionBarReactionPickerRequested);

    auto *highlightFade = new QVariantAnimation(this);
    highlightFade->setDuration(1000);
    highlightFade->setStartValue(1.0);
    highlightFade->setEndValue(0.0);
    highlightFade->setEasingCurve(QEasingCurve::OutQuad);
    connect(highlightFade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        highlightAlpha = value.toReal();
        int row = highlightedRow();
        if (row >= 0)
            update(model()->index(row, 0));
    });

    highlightAnimation = new QSequentialAnimationGroup(this);
    highlightAnimation->addPause(1200);
    highlightAnimation->addAnimation(highlightFade);
    connect(highlightAnimation, &QAbstractAnimation::finished, this, [this]() {
        highlightedMessageId = Core::Snowflake::Invalid;
        highlightAlpha = 0.0;
        viewport()->update();
    });

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &ChatView::onScrollBarValueChanged);
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, &ChatView::updateJumpToPresentBar);
}

bool ChatView::hasTextSelection() const
{
    return selectionAnchor.isValid() && selectionHead.isValid() && selectionAnchor != selectionHead;
}

ChatCursor ChatView::selectionStart() const
{
    return (selectionAnchor < selectionHead) ? selectionAnchor : selectionHead;
}

ChatCursor ChatView::selectionEnd() const
{
    return (selectionAnchor < selectionHead) ? selectionHead : selectionAnchor;
}

void ChatView::setModel(QAbstractItemModel *model)
{
    QListView::setModel(model);

    video->attachModel(model);
    animator->attachModel(model);

    connect(model, &QAbstractItemModel::modelReset, this, &ChatView::onModelReset);
    if (auto *chatModel = qobject_cast<ChatModel *>(model))
        connect(chatModel, &ChatModel::atLatestChanged, this, &ChatView::onAtLatestChanged);

    connect(model, &QAbstractItemModel::rowsAboutToBeInserted, this,
            &ChatView::onRowsAboutToBeInserted);
    connect(model, &QAbstractItemModel::rowsInserted, this, &ChatView::onRowsInserted);
    connect(model, &QAbstractItemModel::dataChanged, this, &ChatView::onDataChanged);
}

void ChatView::resizeEvent(QResizeEvent *event)
{
    video->invalidateRects();
    animator->invalidateRects();

    QListView::resizeEvent(event);
    positionJumpToPresentBar();
}

void ChatView::updateGeometries()
{
    QListView::updateGeometries();
    updateHoveredMessage();
}

void ChatView::paintEvent(QPaintEvent *event)
{
    video->setPaintDamage(event->rect());
    animator->setPaintDamage(event->region());
    QListView::paintEvent(event);
    video->setPaintDamage(QRect());
    animator->setPaintDamage(QRegion());
}

void ChatView::showEvent(QShowEvent *event)
{
    QListView::showEvent(event);
    animator->setViewVisible(true);
}

void ChatView::hideEvent(QHideEvent *event)
{
    animator->setViewVisible(false);
    QListView::hideEvent(event);
    updateHoveredMessage();
}

void ChatView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        QPoint pos = event->pos();
        QModelIndex idx = indexAt(pos);
        auto resolved = ChatLayout::resolveLayout(this, idx);
        auto region = ChatLayout::hitTest(resolved, pos);

        const auto target = region ? MediaTargets::forRegion(resolved, *region) : MediaTarget();
        if (target.isValid()) {
            clearSelection();
            video->press(target, pos);
            return;
        }

        int charPos = ChatLayout::hitTestCharIndex(resolved, pos);

        if (charPos >= 0) {
            selectionAnchor = { idx.row(), charPos };
            selectionHead = selectionAnchor;
            viewport()->update();
        } else {
            clearSelection();
        }
    }
    QListView::mousePressEvent(event);
}

void ChatView::mouseMoveEvent(QMouseEvent *event)
{
    QPoint pos = event->pos();

    if (video->dragging()) {
        video->updateDrag(pos);
        return;
    }

    actionBar->setShiftHeld(event->modifiers().testFlag(Qt::ShiftModifier));
    releaseHoverHold(event->globalPos());
    if (hoveredMessage != messageUnderCursor(event->globalPos()))
        updateHoveredMessage();

    QModelIndex idx = indexAt(pos);

    bool inSelectionDrag = (event->buttons() & Qt::LeftButton) && selectionAnchor.isValid();
    if (inSelectionDrag) {
        int currentRow = idx.isValid() ? idx.row() : (model()->rowCount() - 1);
        if (currentRow < 0)
            return;

        if (!idx.isValid())
            idx = model()->index(currentRow, 0);
    }

    ChatLayout::ResolvedLayout resolved = ChatLayout::resolveLayout(this, idx);

    if (inSelectionDrag) {
        const QRect &textRect = resolved.layout.textRect;

        int newChar = -1;

        if (pos.y() < textRect.top()) {
            newChar = 0;
        } else if (pos.y() > textRect.bottom()) {
            QString content = idx.data(ChatModel::ContentRole).toString();
            newChar = content.length();
        } else {
            if (pos.x() < textRect.left()) {
                newChar = 0;
            } else if (pos.x() > textRect.right()) {
                QString content = idx.data(ChatModel::ContentRole).toString();
                newChar = content.length();
            } else {
                newChar = ChatLayout::hitTestCharIndex(resolved, pos);
            }
        }

        if (newChar >= 0) {
            selectionHead = { idx.row(), newChar };
            viewport()->update();
        }
    }

    auto region = ChatLayout::hitTest(resolved, pos);

    video->updateHover(region ? MediaTargets::forRegion(resolved, *region) : MediaTarget(), pos);

    Qt::CursorShape shape = Qt::ArrowCursor;
    int charPos = -1;
    if (region) {
        if (region->kind == ChatLayout::HitRegion::Kind::TextCursor) {
            shape = Qt::IBeamCursor;
            charPos = ChatLayout::hitTestCharIndex(resolved, pos);
        } else if (region->kind != ChatLayout::HitRegion::Kind::Sticker) {
            shape = Qt::PointingHandCursor;
        }
    }
    if (viewport()->cursor().shape() != shape)
        viewport()->setCursor(shape);

    bool overReplyBar = region && region->kind == ChatLayout::HitRegion::Kind::ReplyBar;
    if (hoveredRow != idx.row() || hoveredChar != charPos || hoveredReplyBar != overReplyBar) {
        if (hoveredRow != -1)
            update(visualRect(model()->index(hoveredRow, 0)));
        hoveredRow = idx.row();
        hoveredChar = charPos;
        hoveredReplyBar = overReplyBar;
        if (hoveredRow != -1)
            update(visualRect(idx));
    }

    QListView::mouseMoveEvent(event);
}

void ChatView::openLink(const QString &url)
{
    if (url.isEmpty())
        return;

    const QLatin1String channelMention("acheron://channel/");
    if (url.startsWith(channelMention)) {
        bool ok = false;
        quint64 id = url.mid(channelMention.size()).toULongLong(&ok);
        if (ok)
            emit channelMentionClicked(Core::Snowflake(id));
        return;
    }

    if (auto link = Discord::ChannelLink::parse(url)) {
        if (link->messageId.isValid())
            emit messageLinkClicked(link->channelId, link->messageId);
        else
            emit channelMentionClicked(link->channelId);
        return;
    }

    auto *confirm = new ConfirmPopup(tr("External Link"), QString(tr("Are you sure you want to open <b>%1</b>?")).arg(url), tr("Open Link"), this);
    confirm->setAttribute(Qt::WA_DeleteOnClose);
    connect(confirm, &QDialog::accepted, this, [url]() { QDesktopServices::openUrl(QUrl(url)); });
    confirm->open();
}

void ChatView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QListView::mouseReleaseEvent(event);
        return;
    }

    QPoint pos = event->pos();

    if (video->dragging()) {
        video->endDrag();
        return;
    }

    QModelIndex idx = indexAt(pos);
    ChatLayout::ResolvedLayout resolved = ChatLayout::resolveLayout(this, idx);
    auto region = ChatLayout::hitTest(resolved, pos);

    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!region || !chatModel) {
        QListView::mouseReleaseEvent(event);
        return;
    }

    using Kind = ChatLayout::HitRegion::Kind;

    auto openImage = [this, chatModel](const MediaHit &hit) {
        auto *viewer = new ImageViewer(imageManager, chatModel->getAccountId(), window());
        viewer->showImage(hit.imageUrl, hit.preview);
    };

    switch (region->kind) {
    case Kind::Reaction: {
        if (hasTextSelection())
            break;
        if (region->index < 0 || region->index >= resolved.ctx.reactions.size())
            break;
        Snowflake channelId = chatModel->getActiveChannelId();
        Snowflake messageId = idx.data(ChatModel::MessageIdRole).toULongLong();
        const ReactionData &r = resolved.ctx.reactions[region->index];
        const Discord::Emoji emoji = r.emojiId.isValid() ? Discord::Emoji::custom(r.emojiId, r.emojiName, r.emojiAnimated) : Discord::Emoji::unicode(r.emojiName);
        emit reactionToggleRequested(channelId, messageId, emoji, r.me, r.isBurst, Discord::Client::ReactionLocation::InlineButton);
        break;
    }

    case Kind::AttachmentVideo:
    case Kind::AttachmentAudio: {
        const auto target = MediaTargets::forRegion(resolved, *region);
        if (!target.isValid())
            break;

        if (target.spoilered) {
            chatModel->revealSpoiler(target.attachmentId);
            break;
        }

        video->release(target, idx, pos);
        break;
    }

    case Kind::AttachmentImage:
    case Kind::AttachmentFile: {
        MediaHit hit = mediaAt(resolved, *region, *chatModel);
        if (hit.spoilerHidden) {
            chatModel->revealSpoiler(hit.attachmentId);
        } else if (hit.isImage()) {
            openImage(hit);
        } else if (hit.isFile()) {
            ConfirmPopup dialog(tr("Open File"),
                                QString(tr("Do you want to open <b>%1</b> (%2) in your browser?"))
                                        .arg(hit.filename)
                                        .arg(ChatLayout::formatFileSize(hit.fileSizeBytes)),
                                tr("Open"), this);
            if (dialog.exec() == QDialog::Accepted)
                QDesktopServices::openUrl(hit.fileUrl);
        }
        break;
    }

    case Kind::EmbedThumbnail: {
        MediaHit hit = mediaAt(resolved, *region, *chatModel);
        if (hit.isImage())
            openImage(hit);
        else
            openLink(region->url);
        break;
    }

    case Kind::EmbedImage: {
        MediaHit hit = mediaAt(resolved, *region, *chatModel);
        if (hit.isImage())
            openImage(hit);
        break;
    }

    case Kind::EmbedVideoThumbnail: {
        const auto target = MediaTargets::forRegion(resolved, *region);
        if (target.isValid())
            video->release(target, idx, pos);
        else
            openLink(region->url);
        break;
    }

    case Kind::EmbedAuthor:
    case Kind::EmbedTitle:
    case Kind::EmbedLink:
        openLink(region->url);
        break;

    case Kind::ForwardOrigin:
    case Kind::TextLink:
        openLink(region->url);
        break;

    case Kind::ReplyBar:
        if (!hasTextSelection() && resolved.ctx.replyData.referencedMessageId.isValid())
            jumpToMessage(resolved.ctx.replyData.referencedMessageId);
        break;

    case Kind::TextCursor:
    case Kind::Avatar:
    case Kind::UsernameHeader:
    case Kind::EmbedDescription:
    case Kind::EmbedFieldName:
    case Kind::EmbedFieldValue:
    case Kind::Sticker:
        break;
    }

    QListView::mouseReleaseEvent(event);
}

void ChatView::clearSelection()
{
    if (selectionAnchor.isValid()) {
        selectionAnchor = { -1, -1 };
        selectionHead = { -1, -1 };
        viewport()->update();
    }
}

void ChatView::leaveEvent(QEvent *event)
{
    bool needsUpdate = (hoveredRow != -1);
    hoveredRow = -1;
    hoveredChar = -1;
    hoveredReplyBar = false;

    if (!video->dragging())
        video->clearHover();

    if (needsUpdate) {
        viewport()->update();
    }

    viewport()->unsetCursor();
    QListView::leaveEvent(event);
}

void ChatView::wheelEvent(QWheelEvent *event)
{
    hoverHeldAt.reset();
    QListView::wheelEvent(event);
}

bool ChatView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::Leave) {
        releaseHoverHold(QCursor::pos());
        updateHoveredMessage();
    }

    if (event->type() == QEvent::ToolTip) {
        auto *helpEvent = static_cast<QHelpEvent *>(event);
        QModelIndex idx = indexAt(helpEvent->pos());
        const ChatLayout::ResolvedLayout resolved = ChatLayout::resolveLayout(this, idx);

        const auto region = ChatLayout::hitTest(resolved, helpEvent->pos());
        if (region && region->kind == ChatLayout::HitRegion::Kind::Sticker) {
            QToolTip::showText(helpEvent->globalPos(), resolved.ctx.stickers[region->index].name, viewport(), region->rect);
            return true;
        }

        QDateTime editedTime = idx.data(ChatModel::EditedTimestampRole).toDateTime();
        if (editedTime.isValid()) {
            auto markerRect = ChatLayout::editedMarkerRectAt(resolved, helpEvent->pos());
            if (markerRect) {
                QToolTip::showText(helpEvent->globalPos(),
                                   tr("Edited %1").arg(Core::TimeUtils::absoluteTime(editedTime)),
                                   viewport(), *markerRect);
                return true;
            }
        }
    }

    return QListView::viewportEvent(event);
}

void ChatView::onHistoryRequestFinished()
{
    isFetchingTop = false;
}

void ChatView::onFutureRequestFinished(bool loadedMore)
{
    isFetchingBottom = false;

    if (loadedMore)
        QTimer::singleShot(0, this, &ChatView::maybeRequestFuture);
}

void ChatView::maybeRequestFuture()
{
    auto *bar = verticalScrollBar();
    if (modelAtLatest() || isFetchingBottom || bar->maximum() - bar->value() >= LoadMoreThreshold)
        return;
    isFetchingBottom = true;
    emit futureRequested();
}

void ChatView::onModelReset()
{
    isFetchingTop = false;
    isFetchingBottom = false;
    anchorIndex = QPersistentModelIndex();

    auto *chatModel = qobject_cast<ChatModel *>(model());
    bool hasJumpTarget = pendingJumpMessageId.isValid() && chatModel &&
                         chatModel->rowForMessage(pendingJumpMessageId) >= 0;
    if (!hasJumpTarget)
        pendingJumpMessageId = Core::Snowflake::Invalid;

    setAtBottom(!hasJumpTarget && modelAtLatest());

    QTimer::singleShot(0, this, [this]() {
        Core::Snowflake target = pendingJumpMessageId;
        pendingJumpMessageId = Core::Snowflake::Invalid;
        if (target.isValid() && scrollToMessage(target)) {
            maybeRequestFuture();
            return;
        }
        scrollToBottom();
        setAtBottom(modelAtLatest());
        updateJumpToPresentBar();
    });
}

void ChatView::onAtLatestChanged(bool atLatest)
{
    if (atLatest) {
        updateScrollState();
        return;
    }
    setAtBottom(false);
    updateJumpToPresentBar();
}

void ChatView::onRowsAboutToBeInserted(const QModelIndex &parent, int start, int end)
{
    if (start == 0) {
        QPoint topPoint(5, 5);
        QModelIndex topVisible = indexAt(topPoint);

        if (topVisible.isValid()) {
            anchorIndex = QPersistentModelIndex(topVisible);
            anchorDistanceFromBottom = visualRect(topVisible).bottom();
        }
    }
}

void ChatView::onRowsInserted(const QModelIndex &parent, int start, int end)
{
    if (atBottom) {
        scrollToBottom();
    } else if (start == 0 && anchorIndex.isValid()) {
        setUpdatesEnabled(false);

        QTimer::singleShot(0, this, [this]() {
            if (!anchorIndex.isValid()) {
                setUpdatesEnabled(true);
                return;
            }

            scrollTo(anchorIndex, QAbstractItemView::PositionAtTop);
            QRect newRect = visualRect(anchorIndex);
            int diff = newRect.bottom() - anchorDistanceFromBottom;
            verticalScrollBar()->setValue(verticalScrollBar()->value() + diff);

            anchorIndex = QPersistentModelIndex();
            isFetchingTop = false;
            setUpdatesEnabled(true);
        });
    }
}

void ChatView::onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight)
{
    const bool hoveredMessageChanged = hoveredMessage.isValid() && topLeft.row() <= hoveredMessage.row() && hoveredMessage.row() <= bottomRight.row();
    if (hoveredMessageChanged)
        updateHoveredMessage();

    if (!atBottom)
        return;

    int lastRow = model()->rowCount() - 1;
    if (lastRow < 0 || bottomRight.row() < lastRow)
        return;

    scheduleDelayedItemsLayout();
    scrollToBottom();
}

void ChatView::setAtBottom(bool value)
{
    if (atBottom == value)
        return;
    atBottom = value;
    emit atBottomChanged(value);
}

void ChatView::onScrollBarValueChanged(int)
{
    updateScrollState();
    updateHoveredMessage();

    if (underMouse())
        video->refreshHoverAt(viewport()->mapFromGlobal(QCursor::pos()));
}

void ChatView::updateScrollState()
{
    auto *bar = verticalScrollBar();

    setAtBottom(bar->value() >= bar->maximum() && modelAtLatest());

    if (bar->value() < LoadMoreThreshold && !isFetchingTop) {
        isFetchingTop = true;
        emit historyRequested();
    }

    maybeRequestFuture();
    updateJumpToPresentBar();
}

bool ChatView::modelAtLatest() const
{
    auto *chatModel = qobject_cast<const ChatModel *>(model());
    return !chatModel || chatModel->isAtLatest();
}

int ChatView::highlightedRow() const
{
    if (!highlightedMessageId.isValid())
        return -1;
    auto *chatModel = qobject_cast<const ChatModel *>(model());
    return chatModel ? chatModel->rowForMessage(highlightedMessageId) : -1;
}

bool ChatView::scrollToMessage(Core::Snowflake messageId)
{
    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel)
        return false;

    int row = chatModel->rowForMessage(messageId);
    if (row < 0)
        return false;

    scrollTo(chatModel->index(row, 0), QAbstractItemView::PositionAtCenter);
    flashMessage(messageId);
    return true;
}

void ChatView::flashMessage(Core::Snowflake messageId)
{
    highlightedMessageId = messageId;
    highlightAlpha = 1.0;
    highlightAnimation->stop();
    highlightAnimation->start();
    viewport()->update();
}

void ChatView::jumpToMessage(Core::Snowflake messageId)
{
    if (!messageId.isValid() || scrollToMessage(messageId))
        return;

    pendingJumpMessageId = messageId;
    emit jumpRequested(messageId);
}

void ChatView::jumpToPresent()
{
    pendingJumpMessageId = Core::Snowflake::Invalid;

    if (modelAtLatest())
        scrollToBottom();
    else
        emit presentRequested();
}

void ChatView::positionJumpToPresentBar()
{
    constexpr int MinWidth = 284;
    constexpr int SideMargin = 16;
    constexpr int BottomMargin = 8;

    jumpToPresentBar->ensurePolished();
    QSize hint = jumpToPresentBar->sizeHint();
    QRect area = viewport()->geometry();
    int width = qBound(qMin(MinWidth, area.width() - 2 * SideMargin),
                       qMax(MinWidth, hint.width()),
                       area.width() - 2 * SideMargin);
    jumpToPresentBar->setGeometry(area.left() + (area.width() - width) / 2,
                                  area.bottom() + 1 - hint.height() - BottomMargin,
                                  width,
                                  hint.height());
}

void ChatView::updateJumpToPresentBar()
{
    constexpr int ScrolledUpThreshold = 120;

    auto *bar = verticalScrollBar();
    bool scrolledUp = bar->maximum() - bar->value() > ScrolledUpThreshold;
    bool show = !modelAtLatest() || scrolledUp;
    if (show == jumpToPresentBar->isVisible())
        return;
    if (show) {
        positionJumpToPresentBar();
        jumpToPresentBar->raise();
    }
    jumpToPresentBar->setVisible(show);
}

bool ChatView::canShowActionBar(const QModelIndex &index) const
{
    return index.isValid() &&
           index.row() != editingRow() &&
           !index.data(ChatModel::IsPendingRole).toBool() &&
           !index.data(ChatModel::IsErroredRole).toBool();
}

bool ChatView::canDeleteMessage(const QModelIndex &index) const
{
    Core::Snowflake authorId = index.data(ChatModel::UserIdRole).toULongLong();
    return authorId == currentUserId || canManageMessages;
}

QModelIndex ChatView::messageUnderCursor(const QPoint &globalPos) const
{
    if (!isVisible())
        return {};

    if (QApplication::activePopupWidget() || hoverHeldAt)
        return hoveredMessage;
    if (QApplication::activeModalWidget())
        return {};

    QWidget *under = QApplication::widgetAt(globalPos);
    if (!under || !viewport()->isAncestorOf(under))
        return {};
    if (actionBar->isAncestorOf(under))
        return hoveredMessage;
    return indexAt(viewport()->mapFromGlobal(globalPos));
}

QPoint ChatView::actionBarPosition(const QModelIndex &index) const
{
    constexpr int AboveHeader = 16;
    constexpr int AboveLine = 25;
    constexpr int RightInset = 14;

    const ChatLayout::MessageLayout layout = ChatLayout::resolveLayout(this, index).layout;
    int top = layout.textRect.top() - AboveLine;
    if (layout.hasReply)
        top = layout.replyRect.top() - AboveLine;
    else if (layout.showHeader)
        top = layout.avatarRect.top() - AboveHeader;

    const QRect bounds = viewport()->rect();
    const QSize size = actionBar->size();
    const int left = qMax(bounds.left(), layout.rowRect.right() + 1 - RightInset - size.width());
    top = qMax(bounds.top(), qMin(top, bounds.bottom() + 1 - size.height()));
    return QPoint(left, top);
}

void ChatView::updateHoveredMessage()
{
    if (!actionBar)
        return;

    const QModelIndex hovered = messageUnderCursor(QCursor::pos());
    if (hoveredMessage != hovered) {
        update(hoveredMessage);
        hoveredMessage = hovered;
        update(hoveredMessage);
    }

    if (!canShowActionBar(hovered)) {
        actionBarMessageId = Core::Snowflake::Invalid;
        actionBar->hide();
        return;
    }

    actionBarMessageId = hovered.data(ChatModel::MessageIdRole).toULongLong();
    actionBar->setCanDelete(canDeleteMessage(hovered));
    actionBar->setReactions(canReactTo(hovered), quickReactionsFor(hovered));
    actionBar->adjustSize();
    actionBar->move(actionBarPosition(hovered));
    actionBar->show();
    actionBar->raise();
}

void ChatView::addMessageAction(QMenu &menu, MessageActionBar::Action action, Core::Snowflake messageId)
{
    QAction *menuAction = menu.addAction(MessageActionBar::label(action));
    connect(menuAction, &QAction::triggered, this, [this, action, messageId]() {
        triggerMessageAction(action, messageId);
    });
}

void ChatView::triggerMessageAction(MessageActionBar::Action action, Core::Snowflake messageId)
{
    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel)
        return;

    switch (action) {
    case MessageActionBar::Action::CopyId:
        copyMessageId(messageId);
        break;
    case MessageActionBar::Action::CopyLink:
        copyMessageLink(messageId);
        break;
    case MessageActionBar::Action::Reply:
        emit replyToMessageRequested(chatModel->getActiveChannelId(), messageId);
        break;
    case MessageActionBar::Action::Delete:
        emit deleteMessageRequested(chatModel->getActiveChannelId(), messageId);
        break;
    }
}

void ChatView::onActionBarTriggered(MessageActionBar::Action action)
{
    triggerMessageAction(action, actionBarMessageId);
}

void ChatView::onActionBarMoreRequested(const QPoint &buttonTopLeft)
{
    using Action = MessageActionBar::Action;
    constexpr int MenuGap = 4;

    QMenu menu(this);

    menu.setAttribute(Qt::WA_NoMouseReplay);
    addMessageAction(menu, Action::Reply, actionBarMessageId);
    addMessageAction(menu, Action::CopyLink, actionBarMessageId);
    if (canDeleteMessage(hoveredMessage))
        addMessageAction(menu, Action::Delete, actionBarMessageId);
    menu.addSeparator();
    addMessageAction(menu, Action::CopyId, actionBarMessageId);

    actionBar->setMoreButtonDown(true);

    execMessageMenu(menu, buttonTopLeft - QPoint(menu.sizeHint().width() + MenuGap, 0));
    actionBar->setMoreButtonDown(false);
}

void ChatView::onActionBarQuickReaction(const Core::PickerEmoji &emoji, bool reacted)
{
    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel || !actionBarMessageId.isValid())
        return;
    emit reactionToggleRequested(chatModel->getActiveChannelId(), actionBarMessageId, emoji.toReactionEmoji(), reacted, false, Discord::Client::ReactionLocation::HoverBar);
}

void ChatView::onActionBarReactionPickerRequested()
{
    requestReactionPicker(actionBarMessageId);
}

bool ChatView::canReactTo(const QModelIndex &index) const
{
    return canAddReactions && emojis && canShowActionBar(index);
}

QList<MessageActionBar::QuickReaction> ChatView::quickReactionsFor(const QModelIndex &index) const
{
    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel || !canReactTo(index))
        return {};

    const auto reactions = index.data(ChatModel::ReactionsRole).value<QList<ReactionData>>();
    const auto ownReactionMatches = [&reactions](const Core::PickerEmoji &emoji) {
        return std::any_of(reactions.cbegin(), reactions.cend(), [&emoji](const ReactionData &reaction) {
            if (!reaction.me || reaction.isBurst)
                return false;
            return emoji.isCustom() ? reaction.emojiId == emoji.customId : (!reaction.emojiId.isValid() && reaction.emojiName == emoji.surrogates);
        });
    };

    QList<MessageActionBar::QuickReaction> quick;
    for (const Core::PickerEmoji &emoji : emojis->quickReactions(chatModel->getActiveChannelId(), MessageActionBar::QuickReactionCount))
        quick.append({ emoji, ownReactionMatches(emoji) });
    return quick;
}

QIcon ChatView::reactionMenuIcon(const Core::PickerEmoji &emoji) const
{
    constexpr int IconPx = 18;
    if (!emoji.isCustom())
        return QIcon(EmojiGlyphs::pixmap(emoji.surrogates, IconPx, devicePixelRatioF()));

    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel || !imageManager)
        return {};

    const QUrl url = emojiStillUrl(emoji.customId, devicePixelRatioF());
    const QSize size(IconPx, IconPx);
    const bool downloaded = imageManager->isCached(url, size);
    const QPixmap pixmap = imageManager->get(url, size, chatModel->getAccountId());
    return downloaded ? QIcon(pixmap) : QIcon();
}

void ChatView::addReactionMenu(QMenu &menu, Core::Snowflake messageId)
{
    constexpr int SuggestedReactions = 12;
    using Location = Discord::Client::ReactionLocation;

    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel || !emojis)
        return;
    const Core::Snowflake channelId = chatModel->getActiveChannelId();

    QMenu *reactionMenu = menu.addMenu(tr("Add Reaction"));
    for (const Core::PickerEmoji &emoji : emojis->frequentlyUsed(channelId, Core::EmojiIntention::Reaction).mid(0, SuggestedReactions)) {
        QAction *action = reactionMenu->addAction(reactionMenuIcon(emoji), ":" + emoji.name + ":");
        connect(action, &QAction::triggered, this, [this, channelId, messageId, emoji]() {
            emit reactionToggleRequested(channelId, messageId, emoji.toReactionEmoji(), false, false, Location::ContextMenu);
        });
    }

    reactionMenu->addSeparator();
    QAction *viewMore = reactionMenu->addAction(tr("View More"));
    connect(viewMore, &QAction::triggered, this, [this, messageId]() {
        QMetaObject::invokeMethod(this, [this, messageId]() { requestReactionPicker(messageId); }, Qt::QueuedConnection);
    });
}

void ChatView::requestReactionPicker(Core::Snowflake messageId)
{
    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel || !messageId.isValid())
        return;

    const bool barOnMessage = actionBar->isVisible() && actionBarMessageId == messageId;
    const QRect globalAnchor = barOnMessage ? actionBar->reactionPickerButtonGlobalRect() : QRect(QCursor::pos(), QSize(1, 1));
    emit reactionPickerRequested(chatModel->getActiveChannelId(), messageId, globalAnchor);
}

void ChatView::setReactionPickerOpen(bool open)
{
    actionBar->setReactionPickerOpen(open);
    if (open)
        return;

    hoverHeldAt = QCursor::pos();
    updateHoveredMessage();
}

void ChatView::execMessageMenu(QMenu &menu, const QPoint &globalPos)
{
    hoverHeldAt = QCursor::pos();
    menu.exec(globalPos);
    hoverHeldAt = QCursor::pos();
    updateHoveredMessage();
}

void ChatView::releaseHoverHold(const QPoint &globalPos)
{
    if (hoverHeldAt && *hoverHeldAt != globalPos && !QApplication::activePopupWidget())
        hoverHeldAt.reset();
}

JumpToPresentBar::JumpToPresentBar(QWidget *parent) : QWidget(parent)
{
    setObjectName("jumpToPresentBar");
    setAttribute(Qt::WA_StyledBackground);
    setCursor(Qt::PointingHandCursor);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 6, 6, 6);
    layout->setSpacing(12);

    label = new QLabel(tr("You're viewing older messages"), this);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(label, 1);

    button = new QPushButton(tr("Jump To Present"), this);
    button->setObjectName("jumpToPresentButton");
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(button, 0);
    connect(button, &QPushButton::clicked, this, &JumpToPresentBar::clicked);

    applyTheme();
    connect(&Core::Theme::Manager::instance(), &Core::Theme::Manager::themeChanged, this, &JumpToPresentBar::applyTheme);
}

void JumpToPresentBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit clicked();
    QWidget::mousePressEvent(event);
}

void JumpToPresentBar::applyTheme()
{
    using namespace Core::Theme;
    const auto &theme = Manager::instance();
    QColor surface = theme.color(Token::ButtonBg);
    QColor border = theme.color(Token::Divider);
    QColor text = theme.color(Token::PrimaryText);
    QColor accent = theme.color(Token::Highlight);
    QColor accentText = theme.color(Token::HighlightedText);

    button->setIcon(Icons::icon(Icons::Name::ArrowDown, Token::HighlightedText));
    setStyleSheet(QStringLiteral(
                          "#jumpToPresentBar { background: %1; border: 1px solid %2; border-radius: 8px; }"
                          "#jumpToPresentBar QLabel { color: %3; font-weight: 500; background: transparent; }"
                          "#jumpToPresentButton { background: %4; color: %5; border: none; border-radius: 4px; padding: 4px 10px; font-weight: 600; }"
                          "#jumpToPresentButton:hover { background: %6; }")
                          .arg(surface.name(), border.name(), text.name(), accent.name(), accentText.name(), accent.lighter(115).name()));
    adjustSize();
}

void ChatView::setCurrentUserId(Core::Snowflake userId)
{
    currentUserId = userId;
}

void ChatView::setCanPinMessages(bool canPin)
{
    canPinMessages = canPin;
}

void ChatView::setCanManageMessages(bool canManage)
{
    canManageMessages = canManage;
}

void ChatView::setCanAddReactions(bool canReact)
{
    canAddReactions = canReact;
    updateHoveredMessage();
}

void ChatView::setEmojiManager(Core::EmojiManager *manager, Core::Snowflake accountId)
{
    emojis = manager;
    actionBar->setImageSource(imageManager, accountId);
    updateHoveredMessage();
}

void ChatView::contextMenuEvent(QContextMenuEvent *event)
{
    QModelIndex index = indexAt(event->pos());
    if (!index.isValid())
        return;

    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel)
        return;

    Core::Snowflake messageId = index.data(ChatModel::MessageIdRole).toULongLong();
    Core::Snowflake authorId = index.data(ChatModel::UserIdRole).toULongLong();
    Core::Snowflake channelId = chatModel->getActiveChannelId();
    QString content = index.data(ChatModel::ContentRole).toString();
    bool isOwnMessage = (authorId == currentUserId);

    ChatLayout::ResolvedLayout resolved = ChatLayout::resolveLayout(this, index);
    auto region = ChatLayout::hitTest(resolved, event->pos());
    if (region && (region->kind == ChatLayout::HitRegion::Kind::Avatar || region->kind == ChatLayout::HitRegion::Kind::UsernameHeader)) {
        emit userContextMenuRequested(authorId, event->globalPos());
        return;
    }

    QMenu menu(this);

    MediaHit hit = region ? mediaAt(resolved, *region, *chatModel) : MediaHit();
    if (hit.isImage() && !hit.spoilerHidden) {
        QAction *copyImageAction = menu.addAction(tr("Copy Image"));
        connect(copyImageAction, &QAction::triggered, this, [this, hit]() {
            copyImage(hit.imageUrl, hit.preview);
        });
    }
    if (hit.isFile()) {
        QAction *saveAction = menu.addAction(tr("Save As..."));
        connect(saveAction, &QAction::triggered, this, [this, hit]() {
            saveMedia(hit.fileUrl, hit.filename);
        });
    }
    QString linkUrl = region ? region->url : QString();
    if (hit.hasLink())
        linkUrl = hit.linkUrl.toString(QUrl::FullyEncoded);
    if (!linkUrl.isEmpty() && !linkUrl.startsWith(QLatin1String("acheron://"))) {
        const bool onSticker = region && region->kind == ChatLayout::HitRegion::Kind::Sticker;
        const QString copyLinkLabel = onSticker ? tr("Copy Sticker Link") : hit.isImage() ? tr("Copy Image Link")
                                                                                          : tr("Copy Link");
        QAction *copyLinkAction = menu.addAction(copyLinkLabel);
        connect(copyLinkAction, &QAction::triggered, this, [linkUrl]() {
            QGuiApplication::clipboard()->setText(linkUrl);
        });
    }
    if (!menu.isEmpty())
        menu.addSeparator();

    QAction *copyAction = menu.addAction(tr("Copy Text"));
    copyAction->setShortcut(QKeySequence::Copy);
    if (hasTextSelection()) {
        connect(copyAction, &QAction::triggered, this, [this]() {
            copySelectedText();
        });
    } else {
        connect(copyAction, &QAction::triggered, this, [this, index]() {
            copyMessageContent(index);
        });
    }

    menu.addSeparator();

    addMessageAction(menu, MessageActionBar::Action::Reply, messageId);

    if (isOwnMessage && !index.data(ChatModel::IsSystemMessageRole).toBool() &&
        !index.data(ChatModel::IsForwardedRole).toBool()) {
        QAction *editAction = menu.addAction(tr("Edit Message"));
        connect(editAction, &QAction::triggered, this, [this, index]() {
            startInlineEdit(index);
        });
    }

    if (canDeleteMessage(index))
        addMessageAction(menu, MessageActionBar::Action::Delete, messageId);

    menu.addSeparator();

    if (canPinMessages) {
        QAction *pinAction = menu.addAction(tr("Pin Message"));
        connect(pinAction, &QAction::triggered, this, [this, channelId, messageId]() {
            emit pinMessageRequested(channelId, messageId);
        });
    }

    if (canReactTo(index))
        addReactionMenu(menu, messageId);

    bool isPending = index.data(ChatModel::IsPendingRole).toBool();
    bool hasAttachments = !index.data(ChatModel::AttachmentsRole).isNull();
    if (isOwnMessage && isPending && hasAttachments) {
        menu.addSeparator();
        QAction *cancelAction = menu.addAction(tr("Cancel Upload"));
        connect(cancelAction, &QAction::triggered, this, [this, channelId, messageId]() {
            emit cancelUploadRequested(channelId, messageId);
        });
    }

    menu.addSeparator();
    addMessageAction(menu, MessageActionBar::Action::CopyLink, messageId);
    addMessageAction(menu, MessageActionBar::Action::CopyId, messageId);

    execMessageMenu(menu, event->globalPos());
}

void ChatView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy)) {
        copySelectedText();
        return;
    }
    QListView::keyPressEvent(event);
}

static bool hasLocalFiles(const QMimeData *mime)
{
    if (!mime->hasUrls())
        return false;
    const auto urls = mime->urls();
    return std::any_of(urls.begin(), urls.end(),
                       [](const QUrl &url) { return url.isLocalFile(); });
}

void ChatView::dragEnterEvent(QDragEnterEvent *event)
{
    if (hasLocalFiles(event->mimeData()))
        event->acceptProposedAction();
}

void ChatView::dragMoveEvent(QDragMoveEvent *event)
{
    if (hasLocalFiles(event->mimeData()))
        event->acceptProposedAction();
}

void ChatView::dropEvent(QDropEvent *event)
{
    if (!hasLocalFiles(event->mimeData()))
        return;
    event->acceptProposedAction();
    emit filesDropped(event->mimeData()->urls());
}

void ChatView::copySelectedText()
{
    if (!hasTextSelection())
        return;

    ChatCursor start = selectionStart();
    ChatCursor end = selectionEnd();

    QString selectedText;
    for (int row = start.row; row <= end.row; row++) {
        QModelIndex idx = model()->index(row, 0);
        QString html = idx.data(ChatModel::HtmlRole).toString();

        QTextDocument doc;
        doc.setHtml(html);

        int docLength = doc.characterCount() - 1;
        int startChar = (row == start.row) ? start.index : 0;
        int endChar = (row == end.row) ? end.index : docLength;

        startChar = qBound(0, startChar, docLength);
        endChar = qBound(0, endChar, docLength);

        if (startChar >= endChar && row == start.row && row == end.row)
            continue;

        QTextCursor cursor(&doc);
        cursor.setPosition(startChar);
        cursor.setPosition(endChar, QTextCursor::KeepAnchor);

        QString rowText = cursor.selectedText();
        rowText.replace(QChar(0x2029), '\n');

        if (!selectedText.isEmpty())
            selectedText += '\n';
        selectedText += rowText;
    }

    if (!selectedText.isEmpty())
        QGuiApplication::clipboard()->setText(selectedText);
}

void ChatView::copyMessageContent(const QModelIndex &index)
{
    QString content = index.data(ChatModel::ContentRole).toString();
    if (!content.isEmpty())
        QGuiApplication::clipboard()->setText(content);
}

void ChatView::copyMessageId(Core::Snowflake messageId)
{
    QGuiApplication::clipboard()->setText(QString::number(quint64(messageId)));
}

void ChatView::copyMessageLink(Core::Snowflake messageId)
{
    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel)
        return;

    Discord::ChannelLink link{ chatModel->getActiveGuildId(), chatModel->getActiveChannelId(), messageId };
    QGuiApplication::clipboard()->setText(link.toUrl());
}

void ChatView::copyImage(const QUrl &proxyUrl, const QPixmap &preview)
{
    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel)
        return;

    QUrl fullQualityUrl = Core::ImageManager::fullQualityUrl(proxyUrl);
    imageManager->fetch(fullQualityUrl, chatModel->getAccountId(), this, [preview](const QByteArray &data) {
        QImage image = QImage::fromData(data);
        if (image.isNull())
            image = preview.toImage();
        if (!image.isNull())
            QGuiApplication::clipboard()->setImage(image);
    });
}

void ChatView::saveMedia(const QUrl &url, const QString &filename)
{
    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel)
        return;

    QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QString path = QFileDialog::getSaveFileName(this, tr("Save As"), QDir(downloads).filePath(filename));
    if (!path.isEmpty())
        imageManager->download(url, chatModel->getAccountId(), path);
}

void ChatView::startInlineEdit(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    QString content = index.data(ChatModel::ContentRole).toString();
    Core::Snowflake messageId = index.data(ChatModel::MessageIdRole).toULongLong();

    currentEditingMessageId = messageId;
    currentEditingIndex = index;

    // Invalidate cached size so sizeHint returns the enlarged height
    auto *m = const_cast<QAbstractItemModel *>(index.model());
    m->setData(index, QSize(), ChatModel::CachedSizeRole);

    // Force the view to re-query sizeHint for this row
    scheduleDelayedItemsLayout();

    // Position the edit widget after layout recalculates
    QTimer::singleShot(0, this, [this, content]() {
        if (!currentEditingIndex.isValid())
            return;

        QRect itemRect = visualRect(currentEditingIndex);
        ChatLayout::ResolvedLayout resolved = ChatLayout::resolveLayout(this, currentEditingIndex);
        const QRect &textRect = resolved.layout.textRect;

        int editHeight = qMax(InlineEditMinHeight, itemRect.bottom() - textRect.top() - 4);
        QRect editRect(textRect.left(), textRect.top(), textRect.width(), editHeight);

        inlineEditWidget->setGeometry(editRect);
        inlineEditWidget->setFont(resolved.ctx.font);
        inlineEditWidget->setPlainText(content);
        inlineEditWidget->setVisible(true);
        inlineEditWidget->setFocus();
        inlineEditWidget->selectAll();

        scrollTo(currentEditingIndex, QAbstractItemView::EnsureVisible);
    });
}

void ChatView::editLastOwnMessage()
{
    auto *chatModel = qobject_cast<ChatModel *>(model());
    if (!chatModel || !currentUserId.isValid())
        return;

    for (int row = chatModel->rowCount() - 1; row >= 0; --row) {
        QModelIndex index = chatModel->index(row, 0);
        if (index.data(ChatModel::UserIdRole).toULongLong() != currentUserId)
            continue;
        if (index.data(ChatModel::IsPendingRole).toBool())
            continue;
        if (index.data(ChatModel::IsSystemMessageRole).toBool())
            continue;
        if (index.data(ChatModel::IsForwardedRole).toBool())
            continue;
        startInlineEdit(index);
        return;
    }
}

void ChatView::commitInlineEdit()
{
    if (!currentEditingIndex.isValid())
        return;

    QString newContent = inlineEditWidget->toPlainText().trimmed();
    QString oldContent = currentEditingIndex.data(ChatModel::ContentRole).toString();

    auto *chatModel = qobject_cast<ChatModel *>(model());
    Core::Snowflake channelId = chatModel ? chatModel->getActiveChannelId() : Core::Snowflake::Invalid;
    Core::Snowflake messageId = currentEditingMessageId;

    cancelInlineEdit();

    if (newContent.isEmpty())
        emit deleteMessageRequested(channelId, messageId);
    else if (newContent != oldContent)
        emit editMessageRequested(channelId, messageId, newContent);
}

void ChatView::cancelInlineEdit()
{
    bool wasEditing = currentEditingIndex.isValid();
    inlineEditWidget->setVisible(false);

    QModelIndex editedIndex = currentEditingIndex;
    currentEditingMessageId = Core::Snowflake::Invalid;
    currentEditingIndex = QModelIndex();

    if (wasEditing)
        emit inlineEditFinished();

    // Invalidate cached size so sizeHint returns the normal height
    if (editedIndex.isValid()) {
        auto *m = const_cast<QAbstractItemModel *>(editedIndex.model());
        m->setData(editedIndex, QSize(), ChatModel::CachedSizeRole);
        scheduleDelayedItemsLayout();
    }
}

bool ChatView::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == inlineEditWidget && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Return && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
            commitInlineEdit();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Escape) {
            cancelInlineEdit();
            return true;
        }
        if (moveCursorToTextEdgeFromOuterLine(inlineEditWidget, keyEvent))
            return true;
    }
    return QListView::eventFilter(obj, event);
}

} // namespace UI
} // namespace Acheron
