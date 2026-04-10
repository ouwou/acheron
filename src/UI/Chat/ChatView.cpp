#include "ChatView.hpp"

#include <QMenu>
#include <QTextDocument>
#include <QTextCursor>

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
        QTimer::singleShot(0, this, &ChatView::scrollToBottom);
    });

    connect(model, &QAbstractItemModel::rowsAboutToBeInserted, this,
            &ChatView::onRowsAboutToBeInserted);
    connect(model, &QAbstractItemModel::rowsInserted, this, &ChatView::onRowsInserted);
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

    if (event->buttons() & Qt::LeftButton && selectionAnchor.isValid()) {
        int currentRow = idx.isValid() ? idx.row() : (model()->rowCount() - 1);
        if (currentRow < 0)
            return;

        if (!idx.isValid())
            idx = model()->index(currentRow, 0);

        QRect visualR = visualRect(idx);
        const bool showHeader = idx.data(ChatModel::ShowHeaderRole).toBool();
        const bool hasSeparator = idx.data(ChatModel::DateSeparatorRole).toBool();
        QFont font = ChatLayout::getFontForIndex(this, idx);
        QFontMetrics fm(font);
        QRect textRect = ChatLayout::textRectForRow(visualR, showHeader, fm, hasSeparator);

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
                newChar = ChatLayout::hitTestCharIndex(this, idx, pos);
            }
        }

        if (newChar >= 0) {
            selectionHead = { currentRow, newChar };
            viewport()->update();
        }
    }

    int charPos = ChatLayout::hitTestCharIndex(this, idx, pos);

    QString link = ChatLayout::getLinkAt(this, idx, pos);
    std::optional<AttachmentData> hoveredAtt = ChatLayout::getAttachmentAt(this, idx, pos);
    std::optional<ChatLayout::EmbedHitResult> hoveredEmbed = ChatLayout::getEmbedAt(this, idx, pos);
    std::optional<ChatLayout::ReactionHitResult> hoveredReaction = ChatLayout::getReactionAt(this, idx, pos);

    if (!link.isEmpty() || hoveredAtt.has_value() || hoveredEmbed.has_value() || hoveredReaction.has_value()) {
        viewport()->setCursor(Qt::PointingHandCursor);
    } else {
        if (charPos >= 0) {
            if (viewport()->cursor().shape() != Qt::IBeamCursor)
                viewport()->setCursor(Qt::IBeamCursor);
        } else {
            if (viewport()->cursor().shape() != Qt::ArrowCursor)
                viewport()->setCursor(Qt::ArrowCursor);
        }
    }

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
    if (event->button() == Qt::LeftButton) {
        QPoint pos = event->pos();
        QModelIndex idx = indexAt(pos);

        std::optional<ChatLayout::ReactionHitResult> reactionHit = ChatLayout::getReactionAt(this, idx, pos);
        if (reactionHit.has_value() && !hasTextSelection()) {
            auto *chatModel = qobject_cast<ChatModel *>(model());
            if (chatModel) {
                Snowflake channelId = chatModel->getActiveChannelId();
                Snowflake messageId = idx.data(ChatModel::MessageIdRole).toULongLong();
                const ReactionData &r = reactionHit->reaction;
                QString emojiStr;
                if (r.emojiId.isValid())
                    emojiStr = r.emojiName + ":" + QString::number(r.emojiId);
                else
                    emojiStr = r.emojiName;
                emit toggleReactionClicked(channelId, messageId, emojiStr, r.me, r.isBurst);
            }
            QListView::mouseReleaseEvent(event);
            return;
        }

        std::optional<ChatLayout::EmbedHitResult> embedHit = ChatLayout::getEmbedAt(this, idx, pos);
        if (embedHit.has_value()) {
            switch (embedHit->hitType) {
            case ChatLayout::EmbedHitType::Image: {
                if (!embedHit->image.isNull()) {
                    auto *viewer = new ImageViewer(window());
                    viewer->showImage(QUrl(embedHit->url), embedHit->image);
                }
                QListView::mouseReleaseEvent(event);
                return;
            }
            case ChatLayout::EmbedHitType::VideoThumbnail:
            case ChatLayout::EmbedHitType::Title:
            case ChatLayout::EmbedHitType::Author:
            case ChatLayout::EmbedHitType::Link: {
                if (!embedHit->url.isEmpty()) {
                    ConfirmPopup dialog(tr("External Link"),
                                        QString(tr("Are you sure you want to open <b>%1</b>?"))
                                                .arg(embedHit->url),
                                        tr("Open Link"), this);

                    if (dialog.exec() == QDialog::Accepted)
                        QDesktopServices::openUrl(QUrl(embedHit->url));
                }
                QListView::mouseReleaseEvent(event);
                return;
            }
            default:
                break;
            }
        }

        std::optional<AttachmentData> att = ChatLayout::getAttachmentAt(this, idx, pos);
        if (att.has_value()) {
            if (att->isImage) {
                if (att->isSpoiler) {
                    ChatModel *chatModel = qobject_cast<ChatModel *>(model());
                    if (chatModel && !chatModel->isSpoilerRevealed(att->id)) {
                        chatModel->revealSpoiler(att->id);
                        QListView::mouseReleaseEvent(event);
                        return;
                    }
                }

                auto *viewer = new ImageViewer(window());
                viewer->showImage(att->proxyUrl, att->pixmap);
                QListView::mouseReleaseEvent(event);
                return;
            } else {
                ConfirmPopup dialog(
                        tr("Open File"),
                        QString(tr("Do you want to open <b>%1</b> (%2) in your browser?"))
                                .arg(att->filename)
                                .arg(ChatLayout::formatFileSize(att->fileSizeBytes)),
                        tr("Open"), this);

                if (dialog.exec() == QDialog::Accepted)
                    QDesktopServices::openUrl(att->originalUrl);

                QListView::mouseReleaseEvent(event);
                return;
            }
        }

        QString link = ChatLayout::getLinkAt(this, idx, pos);

        if (!link.isEmpty()) {
            if (link.startsWith("acheron://channel/")) {
                bool ok = false;
                quint64 id = link.mid(18).toULongLong(&ok);
                if (ok)
                    emit channelMentionClicked(Core::Snowflake(id));
            } else {
                ConfirmPopup dialog(tr("External Link"),
                                    QString(tr("Are you sure you want to open <b>%1</b>?")).arg(link),
                                    tr("Open Link"), this);

                if (dialog.exec() == QDialog::Accepted) {
                    QDesktopServices::openUrl(QUrl(link));
                }
            }
        }
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
    QScrollBar *vbar = verticalScrollBar();
    atBottom = (vbar->value() + vbar->pageStep() >= vbar->maximum());

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

void ChatView::onScrollBarValueChanged(int value)
{
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

    QMenu menu(this);

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
        bool showHeader = currentEditingIndex.data(ChatModel::ShowHeaderRole).toBool();
        bool hasSeparator = currentEditingIndex.data(ChatModel::DateSeparatorRole).toBool();
        QFont font = ChatLayout::getFontForIndex(this, currentEditingIndex);
        QFontMetrics fm(font);
        QRect textRect = ChatLayout::textRectForRow(itemRect, showHeader, fm, hasSeparator);

        ReplyData reply = currentEditingIndex.data(ChatModel::ReplyDataRole).value<ReplyData>();
        if (reply.state != ReplyData::State::None)
            textRect.translate(0, ChatLayout::replyBarHeight() - ChatLayout::padding());

        int editHeight = qMax(InlineEditMinHeight, itemRect.bottom() - textRect.top() - 4);
        QRect editRect(textRect.left(), textRect.top(), textRect.width(), editHeight);

        inlineEditWidget->setGeometry(editRect);
        inlineEditWidget->setFont(font);
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
