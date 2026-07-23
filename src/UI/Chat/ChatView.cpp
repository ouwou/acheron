#include "ChatView.hpp"

#include <QMenu>
#include <QTextDocument>
#include <QTextCursor>
#include <QDesktopServices>

#include <algorithm>

#include "UI/Dialogs/ConfirmPopup.hpp"
#include "UI/ImageViewer.hpp"

namespace Acheron {
namespace UI {
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

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            &ChatView::onScrollBarValueChanged);
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

    connect(model, &QAbstractItemModel::modelReset, this, [this]() {
        isFetchingTop = false;
        anchorIndex = QPersistentModelIndex();
        atBottom = true;
        QTimer::singleShot(0, this, &ChatView::scrollToBottom);
    });

    connect(model, &QAbstractItemModel::rowsAboutToBeInserted, this,
            &ChatView::onRowsAboutToBeInserted);
    connect(model, &QAbstractItemModel::rowsInserted, this, &ChatView::onRowsInserted);
    connect(model, &QAbstractItemModel::dataChanged, this, &ChatView::onDataChanged);
}

void ChatView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        QPoint pos = event->pos();
        QModelIndex idx = indexAt(pos);
        int charPos = ChatLayout::hitTestCharIndex(this, idx, pos);

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

    Qt::CursorShape shape = Qt::ArrowCursor;
    int charPos = -1;
    if (region) {
        if (region->kind == ChatLayout::HitRegion::Kind::TextCursor) {
            shape = Qt::IBeamCursor;
            charPos = ChatLayout::hitTestCharIndex(resolved, pos);
        } else {
            shape = Qt::PointingHandCursor;
        }
    }
    if (viewport()->cursor().shape() != shape)
        viewport()->setCursor(shape);

    if (hoveredRow != idx.row() || hoveredChar != charPos) {
        if (hoveredRow != -1)
            update(visualRect(model()->index(hoveredRow, 0)));
        hoveredRow = idx.row();
        hoveredChar = charPos;
        if (hoveredRow != -1)
            update(visualRect(idx));
    }

    QListView::mouseMoveEvent(event);
}

void ChatView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QListView::mouseReleaseEvent(event);
        return;
    }

    QPoint pos = event->pos();
    QModelIndex idx = indexAt(pos);
    ChatLayout::ResolvedLayout resolved = ChatLayout::resolveLayout(this, idx);
    auto region = ChatLayout::hitTest(resolved, pos);

    if (!region) {
        QListView::mouseReleaseEvent(event);
        return;
    }

    using Kind = ChatLayout::HitRegion::Kind;

    auto openExternalLink = [this](const QString &url) {
        if (url.isEmpty())
            return;
        ConfirmPopup dialog(tr("External Link"),
                            QString(tr("Are you sure you want to open <b>%1</b>?")).arg(url),
                            tr("Open Link"), this);
        if (dialog.exec() == QDialog::Accepted)
            QDesktopServices::openUrl(QUrl(url));
    };

    auto openImage = [this](const QUrl &url, const QPixmap &pixmap) {
        auto *viewer = new ImageViewer(window());
        viewer->showImage(url, pixmap);
    };

    switch (region->kind) {
    case Kind::Reaction: {
        if (hasTextSelection())
            break;
        auto *chatModel = qobject_cast<ChatModel *>(model());
        if (!chatModel || region->index < 0 || region->index >= resolved.ctx.reactions.size())
            break;
        Snowflake channelId = chatModel->getActiveChannelId();
        Snowflake messageId = idx.data(ChatModel::MessageIdRole).toULongLong();
        const ReactionData &r = resolved.ctx.reactions[region->index];
        QString emojiStr = r.emojiId.isValid() ? (r.emojiName + ":" + QString::number(r.emojiId))
                                               : r.emojiName;
        emit toggleReactionClicked(channelId, messageId, emojiStr, r.me, r.isBurst);
        break;
    }

    case Kind::AttachmentVideo: {
        if (region->index < 0 || region->index >= resolved.ctx.attachments.size())
            break;
        const AttachmentData &att = resolved.ctx.attachments[region->index];
        if (att.isSpoiler) {
            auto *chatModel = qobject_cast<ChatModel *>(model());
            if (chatModel && !chatModel->isSpoilerRevealed(att.id)) {
                chatModel->revealSpoiler(att.id);
                break;
            }
        }
        auto *player = new VideoPlayer(window());
        player->showVideo(att.proxyUrl);
        break;
    }
    case Kind::AttachmentVideoDownload: {
        if (region->index < 0 || region->index >= resolved.ctx.attachments.size())
            break;
        const AttachmentData &att = resolved.ctx.attachments[region->index];
        QDesktopServices::openUrl(att.originalUrl);
        break;
    }

    case Kind::AttachmentImage:
    case Kind::AttachmentFile: {
        if (region->index < 0 || region->index >= resolved.ctx.attachments.size())
            break;
        const AttachmentData &att = resolved.ctx.attachments[region->index];
        if (att.isImage) {
            if (att.isSpoiler) {
                auto *chatModel = qobject_cast<ChatModel *>(model());
                if (chatModel && !chatModel->isSpoilerRevealed(att.id)) {
                    chatModel->revealSpoiler(att.id);
                    break;
                }
            }
            openImage(att.proxyUrl, att.pixmap);
        } else {
            ConfirmPopup dialog(tr("Open File"),
                                QString(tr("Do you want to open <b>%1</b> (%2) in your browser?"))
                                        .arg(att.filename)
                                        .arg(ChatLayout::formatFileSize(att.fileSizeBytes)),
                                tr("Open"), this);
            if (dialog.exec() == QDialog::Accepted)
                QDesktopServices::openUrl(att.originalUrl);
        }
        break;
    }

    case Kind::EmbedThumbnail: {
        if (region->index < 0 || region->index >= resolved.ctx.embeds.size())
            break;
        const auto &embed = resolved.ctx.embeds[region->index];
        if (!embed.thumbnail.isNull())
            openImage(QUrl(region->url), embed.thumbnail);
        else
            openExternalLink(region->url);
        break;
    }

    case Kind::EmbedImage: {
        if (region->index < 0 || region->index >= resolved.ctx.embeds.size())
            break;
        const auto &embed = resolved.ctx.embeds[region->index];
        if (region->subIndex < 0 || region->subIndex >= embed.images.size())
            break;
        const auto &img = embed.images[region->subIndex];
        openImage(img.url, img.pixmap);
        break;
    }

    case Kind::EmbedVideoThumbnail:
    case Kind::EmbedAuthor:
    case Kind::EmbedTitle:
    case Kind::EmbedLink:
        openExternalLink(region->url);
        break;

    case Kind::TextLink:
        if (region->url.startsWith("acheron://channel/")) {
            bool ok = false;
            quint64 id = region->url.mid(18).toULongLong(&ok);
            if (ok)
                emit channelMentionClicked(Core::Snowflake(id));
        } else {
            openExternalLink(region->url);
        }
        break;

    case Kind::TextCursor:
    case Kind::Avatar:
    case Kind::UsernameHeader:
    case Kind::ReplyBar:
    case Kind::EmbedDescription:
    case Kind::EmbedFieldName:
    case Kind::EmbedFieldValue:
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

    if (needsUpdate) {
        viewport()->update();
    }

    viewport()->unsetCursor();
    QListView::leaveEvent(event);
}

void ChatView::onHistoryRequestFinished()
{
    isFetchingTop = false;
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
    if (!atBottom)
        return;

    int lastRow = model()->rowCount() - 1;
    if (lastRow < 0 || bottomRight.row() < lastRow)
        return;

    scheduleDelayedItemsLayout();
    scrollToBottom();
}

void ChatView::onScrollBarValueChanged(int value)
{
    atBottom = (value >= verticalScrollBar()->maximum());

    if (value < 200 && !isFetchingTop) {
        isFetchingTop = true;
        emit historyRequested();
    }
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

    if (region && !region->url.isEmpty() && !region->url.startsWith(QLatin1String("acheron://"))) {
        QString linkUrl = region->url;
        QAction *copyLinkAction = menu.addAction(tr("Copy Link"));
        connect(copyLinkAction, &QAction::triggered, this, [linkUrl]() {
            QGuiApplication::clipboard()->setText(linkUrl);
        });
        menu.addSeparator();
    }

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

    QAction *replyAction = menu.addAction(tr("Reply"));
    connect(replyAction, &QAction::triggered, this, [this, channelId, messageId]() {
        emit replyToMessageRequested(channelId, messageId);
    });

    if (isOwnMessage) {
        QAction *editAction = menu.addAction(tr("Edit Message"));
        connect(editAction, &QAction::triggered, this, [this, index]() {
            startInlineEdit(index);
        });
    }

    if (isOwnMessage || canManageMessages) {
        QAction *deleteAction = menu.addAction(tr("Delete Message"));
        connect(deleteAction, &QAction::triggered, this, [this, channelId, messageId]() {
            emit deleteMessageRequested(channelId, messageId);
        });
    }

    menu.addSeparator();

    if (canPinMessages) {
        QAction *pinAction = menu.addAction(tr("Pin Message"));
        connect(pinAction, &QAction::triggered, this, [this, channelId, messageId]() {
            emit pinMessageRequested(channelId, messageId);
        });
    }

    QAction *reactAction = menu.addAction(tr("Add Reaction"));
    connect(reactAction, &QAction::triggered, this, [this, channelId, messageId]() {
        emit addReactionRequested(channelId, messageId);
    });

    bool isPending = index.data(ChatModel::IsPendingRole).toBool();
    bool hasAttachments = !index.data(ChatModel::AttachmentsRole).isNull();
    if (isOwnMessage && isPending && hasAttachments) {
        menu.addSeparator();
        QAction *cancelAction = menu.addAction(tr("Cancel Upload"));
        connect(cancelAction, &QAction::triggered, this, [this, channelId, messageId]() {
            emit cancelUploadRequested(channelId, messageId);
        });
    }

    menu.exec(event->globalPos());
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

void ChatView::commitInlineEdit()
{
    if (!currentEditingIndex.isValid())
        return;

    QString newContent = inlineEditWidget->toPlainText().trimmed();
    QString oldContent = currentEditingIndex.data(ChatModel::ContentRole).toString();

    if (newContent != oldContent && !newContent.isEmpty()) {
        auto *chatModel = qobject_cast<ChatModel *>(model());
        Core::Snowflake channelId = chatModel ? chatModel->getActiveChannelId() : Core::Snowflake::Invalid;
        emit editMessageRequested(channelId, currentEditingMessageId, newContent);
    }

    cancelInlineEdit();
}

void ChatView::cancelInlineEdit()
{
    inlineEditWidget->setVisible(false);

    QModelIndex editedIndex = currentEditingIndex;
    currentEditingMessageId = Core::Snowflake::Invalid;
    currentEditingIndex = QModelIndex();

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
    }
    return QListView::eventFilter(obj, event);
}

} // namespace UI
} // namespace Acheron
