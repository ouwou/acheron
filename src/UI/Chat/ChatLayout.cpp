#include "ChatLayout.hpp"

namespace Acheron {
namespace UI {
namespace ChatLayout {

QRect dateSeparatorRectForRow(const QRect &rowRect)
{
    return QRect(rowRect.left(), rowRect.top(), rowRect.width(), separatorHeight());
}

QFont getFontForIndex(const QAbstractItemView *view, const QModelIndex &index)
{
    QFont font = view->font();
    QVariant modelFont = index.data(Qt::FontRole);
    if (modelFont.isValid() && !modelFont.isNull()) {
        font = qvariant_cast<QFont>(modelFont).resolve(font);
    }
    return font;
}

QRect avatarRectForRow(const QRect &rowRect, bool hasSeperator)
{
    int topOffset = hasSeperator ? separatorHeight() : 0;
    return QRect(rowRect.left() + padding(), rowRect.top() + padding() + topOffset, avatarSize(),
                 avatarSize());
}

QRect headerRectForRow(const QRect &rowRect, const QFontMetrics &fm, bool hasSeperator)
{
    int topOffset = hasSeperator ? separatorHeight() : 0;
    int left = rowRect.left() + padding() + avatarSize() + padding();
    int width = rowRect.right() - left - padding();
    return QRect(left, rowRect.top() + padding() + topOffset, width, fm.height());
}

QRect textRectForRow(const QRect &rowRect, bool showHeader, const QFontMetrics &fm,
                     bool hasSeperator)
{
    int topOffset = hasSeperator ? separatorHeight() : 0;

    int left = rowRect.left() + padding() + avatarSize() + padding();
    int width = rowRect.right() - left - padding();
    if (width < 10)
        width = 10;

    int top = rowRect.top() + topOffset;

    if (showHeader) {
        top += padding() + fm.height();
    } else {
        top += 1;
    }

    int height = rowRect.bottom() - top - padding() + 1;
    if (height < 0)
        height = 0;

    return QRect(left, top, width, height);
}

void setupDocument(QTextDocument &doc, const QString &htmlContent, const QFont &font, int textWidth)
{
    doc.setDefaultFont(font);
    doc.setHtml(htmlContent);
    doc.setTextWidth(textWidth);
    doc.setDocumentMargin(0);
    QTextOption opt = doc.defaultTextOption();
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc.setDefaultTextOption(opt);
}

int hitTestCharIndex(QAbstractItemView *view, const QModelIndex &index, const QPoint &viewportPos)
{
    if (!index.isValid() || !view)
        return -1;

    const bool showHeader = index.data(ChatModel::ShowHeaderRole).toBool();
    const QString content = index.data(ChatModel::ContentRole).toString();
    const QString html = index.data(ChatModel::HtmlRole).toString();
    const bool hasSeparator = index.data(ChatModel::DateSeparatorRole).toBool();

    QFont docFont = getFontForIndex(view, index);
    QFontMetrics fm(docFont);

    QRect rowRect = view->visualRect(index);
    QRect textRect = textRectForRow(rowRect, showHeader, fm, hasSeparator);

    QTextDocument doc;
    setupDocument(doc, html, docFont, textRect.width());

    QPointF local = viewportPos - textRect.topLeft();

    if (local.y() < 0 || local.y() > doc.size().height())
        return -1;

    return doc.documentLayout()->hitTest(local, Qt::ExactHit);
}

QRectF charRectInDocument(const QTextDocument &doc, int charIndex)
{
    if (charIndex < 0)
        return QRectF();
    QTextBlock block = doc.findBlock(charIndex);
    if (!block.isValid())
        return QRectF();

    int blockPos = block.position();
    int offset = charIndex - blockPos;
    QTextLayout *layout = block.layout();
    if (!layout)
        return QRectF();

    QTextLine line = layout->lineForTextPosition(offset);
    if (!line.isValid())
        return QRectF();

    qreal x1 = line.cursorToX(offset);
    qreal x2 = line.cursorToX(offset + 1);
    if (qFuzzyCompare(x1, x2))
        x2 = x1 + 6;
    qreal y = doc.documentLayout()->blockBoundingRect(block).top() + line.y();
    return QRectF(x1, y, x2 - x1, line.height());
}

QString getLinkAt(const QAbstractItemView *view, const QModelIndex &index, const QPoint &mousePos)
{
    if (!index.isValid() || !view)
        return {};

    QString html = index.data(ChatModel::HtmlRole).toString();
    if (html.isEmpty())
        return {};

    QRect rowRect = view->visualRect(index);

    bool showHeader = index.data(ChatModel::ShowHeaderRole).toBool();
    bool hasSeparator = index.data(ChatModel::DateSeparatorRole).toBool();
    QFont font = view->font();
    QFontMetrics fm(font);

    QRect textRect = textRectForRow(rowRect, showHeader, fm, hasSeparator);

    if (!textRect.contains(mousePos))
        return {};

    QTextDocument doc;
    setupDocument(doc, html, font, textRect.width());

    QPointF localPos = mousePos - textRect.topLeft();

    return doc.documentLayout()->anchorAt(localPos);
}

AttachmentGridLayout calculateAttachmentGrid(int count, int maxWidth)
{
    AttachmentGridLayout layout;

    if (count <= 0) {
        layout.totalHeight = 0;
        return layout;
    }

    constexpr int MaxGridWidth = 400;
    int gridWidth = std::min(maxWidth, MaxGridWidth);

    constexpr int gap = 4;

    if (count == 1) {
        // placeholder. single images are shown in full
        layout.cells.append({ 0, QRect(0, 0, gridWidth, 300) });
        layout.totalHeight = 300;
    } else if (count == 2) {
        // 2 horizontal
        int w = (gridWidth - gap) / 2;
        layout.cells.append({ 0, QRect(0, 0, w, 300) });
        layout.cells.append({ 1, QRect(w + gap, 0, w, 300) });
        layout.totalHeight = 300;
    } else if (count == 3) {
        // 1 then 2 vertical
        int leftW = (gridWidth * 2) / 3 - gap / 2;
        int rightW = gridWidth / 3 - gap / 2;
        int rightH = (300 - gap) / 2;
        layout.cells.append({ 0, QRect(0, 0, leftW, 300) });
        layout.cells.append({ 1, QRect(leftW + gap, 0, rightW, rightH) });
        layout.cells.append({ 2, QRect(leftW + gap, rightH + gap, rightW, rightH) });
        layout.totalHeight = 300;
    } else if (count == 4) {
        // 2x2
        int w = (gridWidth - gap) / 2;
        int h = 150;
        layout.cells.append({ 0, QRect(0, 0, w, h) });
        layout.cells.append({ 1, QRect(w + gap, 0, w, h) });
        layout.cells.append({ 2, QRect(0, h + gap, w, h) });
        layout.cells.append({ 3, QRect(w + gap, h + gap, w, h) });
        layout.totalHeight = h * 2 + gap;
    } else if (count == 5) {
        // 2 on top, 3 on bottom
        int topW = (gridWidth - gap) / 2;
        int bottomW = (gridWidth - gap * 2) / 3;
        int h = 150;
        layout.cells.append({ 0, QRect(0, 0, topW, h) });
        layout.cells.append({ 1, QRect(topW + gap, 0, topW, h) });
        layout.cells.append({ 2, QRect(0, h + gap, bottomW, h) });
        layout.cells.append({ 3, QRect(bottomW + gap, h + gap, bottomW, h) });
        layout.cells.append({ 4, QRect((bottomW + gap) * 2, h + gap, bottomW, h) });
        layout.totalHeight = h * 2 + gap;
    } else if (count == 6) {
        // 2x3
        int w = (gridWidth - gap * 2) / 3;
        int h = 150;
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col) {
                int idx = row * 3 + col;
                layout.cells.append({ idx, QRect(col * (w + gap), row * (h + gap), w, h) });
            }
        layout.totalHeight = h * 2 + gap;
    } else if (count == 7) {
        // 1 on top, 2x3 on bottom
        int h = 133;
        int w3 = (gridWidth - gap * 2) / 3;
        layout.cells.append({ 0, QRect(0, 0, gridWidth, h) });
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col) {
                int idx = 1 + row * 3 + col;
                layout.cells.append({ idx, QRect(col * (w3 + gap), (row + 1) * (h + gap), w3, h) });
            }
        layout.totalHeight = h * 3 + gap * 2;
    } else if (count == 8) {
        // 2 on top, 2x3 on bottom
        int h = 133;
        int w2 = (gridWidth - gap) / 2;
        int w3 = (gridWidth - gap * 2) / 3;
        layout.cells.append({ 0, QRect(0, 0, w2, h) });
        layout.cells.append({ 1, QRect(w2 + gap, 0, w2, h) });
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col) {
                int idx = 2 + row * 3 + col;
                layout.cells.append({ idx, QRect(col * (w3 + gap), (row + 1) * (h + gap), w3, h) });
            }
        layout.totalHeight = h * 3 + gap * 2;
    } else if (count == 9) {
        // 3x3
        int w = (gridWidth - gap * 2) / 3;
        int h = 133;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col) {
                int idx = row * 3 + col;
                layout.cells.append({ idx, QRect(col * (w + gap), row * (h + gap), w, h) });
            }
        layout.totalHeight = h * 3 + gap * 2;
    } else {
        // 1 on top, 3x3 on bottom
        int h = 133;
        int w3 = (gridWidth - gap * 2) / 3;
        layout.cells.append({ 0, QRect(0, 0, gridWidth, h) });
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col) {
                int idx = 1 + row * 3 + col;
                if (idx >= count)
                    break;
                layout.cells.append({ idx, QRect(col * (w3 + gap), (row + 1) * (h + gap), w3, h) });
            }
        layout.totalHeight = h * 4 + gap * 3;
    }

    return layout;
}

std::optional<AttachmentData> getAttachmentAt(const QAbstractItemView *view,
                                              const QModelIndex &index, const QPoint &mousePos)
{
    if (!index.isValid() || !view)
        return std::nullopt;

    QList<AttachmentData> attachments =
            index.data(ChatModel::AttachmentsRole).value<QList<AttachmentData>>();

    if (attachments.isEmpty())
        return std::nullopt;

    QRect rowRect = view->visualRect(index);
    bool showHeader = index.data(ChatModel::ShowHeaderRole).toBool();
    bool hasSeparator = index.data(ChatModel::DateSeparatorRole).toBool();
    QString html = index.data(ChatModel::HtmlRole).toString();
    QFont font = view->font();
    QFontMetrics fm(font);

    QRect textRect = textRectForRow(rowRect, showHeader, fm, hasSeparator);

    // calculate actual text height
    QTextDocument doc;
    setupDocument(doc, html, font, textRect.width());
    int realTextHeight = int(std::ceil(doc.size().height()));

    int attachmentTop = textRect.top() + realTextHeight + padding();

    QList<AttachmentData> images;
    QList<AttachmentData> files;
    for (const auto &att : attachments) {
        if (att.isImage)
            images.append(att);
        else
            files.append(att);
    }

    if (!images.isEmpty()) {
        bool isSingleImage = (images.size() == 1);

        if (isSingleImage) {
            const auto &att = images[0];
            QRect imgRect(textRect.left(), attachmentTop, att.displaySize.width(),
                          att.displaySize.height());

            if (imgRect.contains(mousePos))
                return att;

            attachmentTop += att.displaySize.height() + padding();
        } else {
            AttachmentGridLayout grid = calculateAttachmentGrid(images.size(), textRect.width());

            for (const auto &cell : grid.cells) {
                if (cell.attachmentIndex >= images.size())
                    continue;

                QRect imgRect = cell.rect.translated(textRect.left(), attachmentTop);

                if (imgRect.contains(mousePos))
                    return images[cell.attachmentIndex];
            }

            attachmentTop += grid.totalHeight + padding();
        }
    }

    constexpr int fileAttachmentHeight = 48;
    constexpr int maxAttachmentWidth = 400;
    int fileWidth = std::min(textRect.width(), maxAttachmentWidth);

    for (const auto &att : files) {
        QRect fileRect(textRect.left(), attachmentTop, fileWidth, fileAttachmentHeight);

        if (fileRect.contains(mousePos))
            return att;

        attachmentTop = fileRect.bottom() + padding();
    }

    return std::nullopt;
}

QString formatFileSize(qint64 bytes)
{
    if (bytes < 0)
        return "0 B";

    constexpr qint64 KB = 1024;
    constexpr qint64 MB = KB * 1024;
    constexpr qint64 GB = MB * 1024;

    if (bytes >= GB)
        return QString::number(bytes / double(GB), 'f', 2) + " GB";
    if (bytes >= MB)
        return QString::number(bytes / double(MB), 'f', 2) + " MB";
    if (bytes >= KB)
        return QString::number(bytes / double(KB), 'f', 2) + " KB";
    return QString::number(bytes) + " B";
}

} // namespace ChatLayout
} // namespace UI
} // namespace Acheron
