#include "ChatView.hpp"

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
        anchorIndex = {};
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

    if (!link.isEmpty() || hoveredAtt.has_value() || hoveredEmbed.has_value()) {
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
            ConfirmPopup dialog(tr("External Link"),
                                QString(tr("Are you sure you want to open <b>%1</b>?")).arg(link),
                                tr("Open Link"), this);

            if (dialog.exec() == QDialog::Accepted) {
                QDesktopServices::openUrl(QUrl(link));
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

            anchorIndex = {};
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
} // namespace UI
} // namespace Acheron
