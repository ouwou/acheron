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

std::optional<EmbedHitResult> getEmbedAt(const QAbstractItemView *view, const QModelIndex &index,
                                         const QPoint &mousePos)
{
    if (!index.isValid() || !view)
        return std::nullopt;

    QList<EmbedData> embeds = index.data(ChatModel::EmbedsRole).value<QList<EmbedData>>();
    if (embeds.isEmpty())
        return std::nullopt;

    QRect rowRect = view->visualRect(index);
    bool showHeader = index.data(ChatModel::ShowHeaderRole).toBool();
    bool hasSeparator = index.data(ChatModel::DateSeparatorRole).toBool();
    QString html = index.data(ChatModel::HtmlRole).toString();
    QFont font = view->font();
    QFontMetrics fm(font);

    QRect textRect = textRectForRow(rowRect, showHeader, fm, hasSeparator);

    QTextDocument doc;
    setupDocument(doc, html, font, textRect.width());
    int realTextHeight = int(std::ceil(doc.size().height()));

    int embedTop = textRect.top() + realTextHeight + padding();

    QList<AttachmentData> attachments =
            index.data(ChatModel::AttachmentsRole).value<QList<AttachmentData>>();
    if (!attachments.isEmpty()) {
        int imageCount = 0;
        int fileCount = 0;
        QSize firstImageSize;
        for (const auto &att : attachments) {
            if (att.isImage) {
                if (imageCount == 0)
                    firstImageSize = att.displaySize;
                imageCount++;
            } else {
                fileCount++;
            }
        }

        if (imageCount == 1)
            embedTop += firstImageSize.height() + padding();
        else if (imageCount > 1) {
            AttachmentGridLayout grid = calculateAttachmentGrid(imageCount, textRect.width());
            embedTop += grid.totalHeight + padding();
        }

        constexpr int fileAttachmentHeight = 48;
        embedTop += fileCount * (fileAttachmentHeight + padding());
    }

    constexpr int embedMaxWidth = 400;
    constexpr int embedBorderWidth = 4;
    constexpr int embedPadding = 12;
    constexpr int thumbnailSize = 80;
    constexpr int authorIconSize = 24;
    constexpr int footerIconSize = 16;
    constexpr int fieldSpacing = 8;

    for (int embedIndex = 0; embedIndex < embeds.size(); ++embedIndex) {
        const auto &embed = embeds[embedIndex];
        int embedWidth = std::min(textRect.width(), embedMaxWidth);
        int contentWidth = embedWidth - embedBorderWidth - embedPadding * 2;

        bool hasThumbnail = !embed.thumbnail.isNull() ||
                            (!embed.videoThumbnail.isNull() && embed.images.isEmpty());
        if (hasThumbnail)
            contentWidth -= (thumbnailSize + embedPadding);

        int embedHeight = embedPadding;

        if (!embed.providerName.isEmpty()) {
            QFont providerFont = font;
            providerFont.setPointSize(providerFont.pointSize() - 2);
            QFontMetrics providerFm(providerFont);
            embedHeight += providerFm.height() + 4;
        }

        if (!embed.authorName.isEmpty()) {
            QFont authorFont = font;
            authorFont.setPointSize(authorFont.pointSize() - 1);
            authorFont.setBold(true);
            QFontMetrics authorFm(authorFont);
            embedHeight += std::max(authorIconSize, authorFm.height()) + 4;
        }

        if (!embed.title.isEmpty()) {
            QFont titleFont = font;
            titleFont.setBold(true);
            QFontMetrics titleFm(titleFont);
            embedHeight += titleFm.height() + 4;
        }

        if (!embed.description.isEmpty()) {
            QTextDocument descDoc;
            descDoc.setDefaultFont(font);
            descDoc.setTextWidth(contentWidth);
            descDoc.setPlainText(embed.description);
            embedHeight += int(std::ceil(descDoc.size().height())) + 8;
        }

        if (!embed.fields.isEmpty()) {
            QFont fieldNameFont = font;
            fieldNameFont.setBold(true);
            QFontMetrics fieldNameFm(fieldNameFont);
            int fieldWidthCalc = (contentWidth - 2 * fieldSpacing) / 3;

            int fieldsInRow = 0;
            int maxRowHeight = 0;
            for (const auto &field : embed.fields) {
                QTextDocument valueDoc;
                valueDoc.setDefaultFont(font);
                valueDoc.setTextWidth(field.isInline ? fieldWidthCalc : contentWidth);
                valueDoc.setPlainText(field.value);
                int valueHeight = int(std::ceil(valueDoc.size().height()));
                int fieldHeight = fieldNameFm.height() + 2 + valueHeight;

                if (field.isInline) {
                    fieldsInRow++;
                    maxRowHeight = std::max(maxRowHeight, fieldHeight);
                    if (fieldsInRow >= 3) {
                        embedHeight += maxRowHeight + fieldSpacing;
                        fieldsInRow = 0;
                        maxRowHeight = 0;
                    }
                } else {
                    if (fieldsInRow > 0) {
                        embedHeight += maxRowHeight + fieldSpacing;
                        fieldsInRow = 0;
                        maxRowHeight = 0;
                    }
                    embedHeight += fieldHeight + fieldSpacing;
                }
            }
            if (fieldsInRow > 0)
                embedHeight += maxRowHeight + fieldSpacing;
        }

        int imageY = embedHeight;

        if (!embed.images.isEmpty()) {
            if (embed.images.size() == 1) {
                const auto &img = embed.images[0];
                if (!img.pixmap.isNull()) {
                    QSize actualSize =
                            img.pixmap.size().scaled(img.displaySize, Qt::KeepAspectRatio);
                    embedHeight += actualSize.height();
                }
            } else {
                AttachmentGridLayout grid =
                        calculateAttachmentGrid(embed.images.size(), contentWidth);
                embedHeight += grid.totalHeight;
            }
        } else if (!embed.videoThumbnail.isNull() && embed.thumbnail.isNull()) {
            QSize actualSize = embed.videoThumbnail.size().scaled(embed.videoThumbnailSize,
                                                                  Qt::KeepAspectRatio);
            embedHeight += actualSize.height();
        }

        if (!embed.footerText.isEmpty()) {
            QFont footerFont = font;
            footerFont.setPointSize(footerFont.pointSize() - 2);
            QFontMetrics footerFm(footerFont);
            embedHeight += std::max(footerIconSize, footerFm.height()) + 4;
        }

        if (hasThumbnail)
            embedHeight = std::max(embedHeight, int(embedPadding * 2 + thumbnailSize));

        // embedHeight += embedPadding;

        QRect embedRect(textRect.left(), embedTop, embedWidth, embedHeight);

        if (embedRect.contains(mousePos)) {
            int contentLeft = embedRect.left() + embedBorderWidth + embedPadding;
            int contentTop = embedRect.top() + embedPadding;
            int currentY = contentTop;

            if (hasThumbnail) {
                QSize thumbSize =
                        !embed.thumbnail.isNull() ? embed.thumbnailSize : embed.videoThumbnailSize;
                int thumbX = contentLeft + contentWidth + embedPadding;
                QRect thumbRect(thumbX, currentY, thumbSize.width(), thumbSize.height());
                if (thumbRect.contains(mousePos)) {
                    EmbedHitResult result;
                    result.embedIndex = embedIndex;
                    result.hitType = !embed.thumbnail.isNull() ? EmbedHitType::Image
                                                               : EmbedHitType::VideoThumbnail;
                    result.image =
                            !embed.thumbnail.isNull() ? embed.thumbnail : embed.videoThumbnail;
                    result.imageSize = thumbSize;
                    result.url = embed.thumbnailUrl.toString();
                    return result;
                }
            }

            if (!embed.providerName.isEmpty()) {
                QFont providerFont = font;
                providerFont.setPointSize(providerFont.pointSize() - 2);
                QFontMetrics providerFm(providerFont);
                currentY += providerFm.height() + 4;
            }

            if (!embed.authorName.isEmpty()) {
                QFont authorFont = font;
                authorFont.setPointSize(authorFont.pointSize() - 1);
                authorFont.setBold(true);
                QFontMetrics authorFm(authorFont);
                int authorHeight = std::max(authorIconSize, authorFm.height());
                QRect authorRect(contentLeft, currentY, contentWidth, authorHeight);
                if (authorRect.contains(mousePos) && !embed.authorUrl.isEmpty()) {
                    EmbedHitResult result;
                    result.embedIndex = embedIndex;
                    result.hitType = EmbedHitType::Author;
                    result.url = embed.authorUrl;
                    return result;
                }
                currentY += authorHeight + 4;
            }

            if (!embed.title.isEmpty()) {
                QFont titleFont = font;
                titleFont.setBold(true);
                QFontMetrics titleFm(titleFont);
                QRect titleRect(contentLeft, currentY, contentWidth, titleFm.height());
                if (titleRect.contains(mousePos) && !embed.url.isEmpty()) {
                    EmbedHitResult result;
                    result.embedIndex = embedIndex;
                    result.hitType = EmbedHitType::Title;
                    result.url = embed.url;
                    return result;
                }
                currentY += titleFm.height() + 4;
            }

            if (!embed.description.isEmpty()) {
                QTextDocument descDoc;
                descDoc.setDefaultFont(font);
                descDoc.setTextWidth(contentWidth);
                descDoc.setPlainText(embed.description);
                currentY += int(std::ceil(descDoc.size().height())) + 8;
            }

            if (!embed.fields.isEmpty()) {
                QFont fieldNameFont = font;
                fieldNameFont.setBold(true);
                QFontMetrics fieldNameFm(fieldNameFont);
                int fieldWidthCalc = (contentWidth - 2 * fieldSpacing) / 3;

                int fieldsInRow = 0;
                int maxRowHeight = 0;
                for (const auto &field : embed.fields) {
                    QTextDocument valueDoc;
                    valueDoc.setDefaultFont(font);
                    valueDoc.setTextWidth(field.isInline ? fieldWidthCalc : contentWidth);
                    valueDoc.setPlainText(field.value);
                    int valueHeight = int(std::ceil(valueDoc.size().height()));
                    int fieldHeight = fieldNameFm.height() + 2 + valueHeight;

                    if (field.isInline) {
                        fieldsInRow++;
                        maxRowHeight = std::max(maxRowHeight, fieldHeight);
                        if (fieldsInRow >= 3) {
                            currentY += maxRowHeight + fieldSpacing;
                            fieldsInRow = 0;
                            maxRowHeight = 0;
                        }
                    } else {
                        if (fieldsInRow > 0) {
                            currentY += maxRowHeight + fieldSpacing;
                            fieldsInRow = 0;
                            maxRowHeight = 0;
                        }
                        currentY += fieldHeight + fieldSpacing;
                    }
                }
                if (fieldsInRow > 0)
                    currentY += maxRowHeight + fieldSpacing;
            }

            if (!embed.images.isEmpty()) {
                bool isSingleImage = (embed.images.size() == 1);

                if (isSingleImage) {
                    const auto &img = embed.images[0];
                    if (!img.pixmap.isNull()) {
                        QSize actualSize =
                                img.pixmap.size().scaled(img.displaySize, Qt::KeepAspectRatio);
                        QRect imageRect(contentLeft, currentY, actualSize.width(),
                                        actualSize.height());
                        if (imageRect.contains(mousePos)) {
                            EmbedHitResult result;
                            result.embedIndex = embedIndex;
                            result.hitType = EmbedHitType::Image;
                            result.image = img.pixmap;
                            result.imageSize = actualSize;
                            result.url = img.url.toString();
                            return result;
                        }
                    }
                } else {
                    AttachmentGridLayout grid =
                            calculateAttachmentGrid(embed.images.size(), contentWidth);
                    for (const auto &cell : grid.cells) {
                        if (cell.attachmentIndex >= embed.images.size())
                            continue;

                        QRect imgRect = cell.rect.translated(contentLeft, currentY);
                        if (imgRect.contains(mousePos)) {
                            const auto &img = embed.images[cell.attachmentIndex];
                            EmbedHitResult result;
                            result.embedIndex = embedIndex;
                            result.hitType = EmbedHitType::Image;
                            result.image = img.pixmap;
                            result.imageSize = imgRect.size();
                            result.url = img.url.toString();
                            return result;
                        }
                    }
                }
            } else if (!embed.videoThumbnail.isNull() && embed.thumbnail.isNull()) {
                QSize actualSize = embed.videoThumbnail.size().scaled(embed.videoThumbnailSize,
                                                                      Qt::KeepAspectRatio);
                QRect videoRect(contentLeft, currentY, actualSize.width(), actualSize.height());
                if (videoRect.contains(mousePos)) {
                    EmbedHitResult result;
                    result.embedIndex = embedIndex;
                    result.hitType = EmbedHitType::VideoThumbnail;
                    result.image = embed.videoThumbnail;
                    result.imageSize = actualSize;
                    result.url = embed.url;
                    return result;
                }
            }
        }

        embedTop += embedHeight + padding();
    }

    return std::nullopt;
}

} // namespace ChatLayout
} // namespace UI
} // namespace Acheron
