#include "ChatLayout.hpp"
#include <QPainter>

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
    if (modelFont.isValid() && !modelFont.isNull())
        font = qvariant_cast<QFont>(modelFont).resolve(font);
    return font;
}

QRect avatarRectForRow(const QRect &rowRect, bool hasSeparator)
{
    int topOffset = hasSeparator ? separatorHeight() : 0;
    return QRect(rowRect.left() + padding(), rowRect.top() + padding() + topOffset, avatarSize(),
                 avatarSize());
}

QRect headerRectForRow(const QRect &rowRect, const QFontMetrics &fm, bool hasSeparator)
{
    int topOffset = hasSeparator ? separatorHeight() : 0;
    int left = rowRect.left() + padding() + avatarSize() + padding();
    int width = rowRect.right() - left - padding();
    return QRect(left, rowRect.top() + padding() + topOffset, width, fm.height());
}

QRect textRectForRow(const QRect &rowRect, bool showHeader, const QFontMetrics &fm,
                     bool hasSeparator)
{
    int topOffset = hasSeparator ? separatorHeight() : 0;
    int left = rowRect.left() + padding() + avatarSize() + padding();
    int width = rowRect.right() - left - padding();
    if (width < 10)
        width = 10;

    int top = rowRect.top() + topOffset;
    if (showHeader)
        top += padding() + fm.height();
    else
        top += 1;

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

EmbedLayout calculateEmbedLayout(const EmbedData &embed, const QFont &font, int maxWidth, int top)
{
    EmbedLayout layout = {};

    int embedWidth = std::min(maxWidth, embedMaxWidth());
    int contentWidth = embedWidth - embedBorderWidth() - embedPadding() * 2;

    layout.hasThumbnail =
            !embed.thumbnail.isNull() || (!embed.videoThumbnail.isNull() && embed.images.isEmpty());
    if (layout.hasThumbnail)
        contentWidth -= (thumbnailSize() + embedPadding());

    layout.contentWidth = contentWidth;

    int currentY = embedPadding();

    layout.providerY = currentY;
    layout.providerHeight = 0;
    if (!embed.providerName.isEmpty()) {
        QFont providerFont = font;
        providerFont.setPointSize(providerFont.pointSize() - 2);
        QFontMetrics providerFm(providerFont);
        layout.providerHeight = providerFm.height() + 4;
        currentY += layout.providerHeight;
    }

    layout.authorY = currentY;
    layout.authorHeight = 0;
    if (!embed.authorName.isEmpty()) {
        QFont authorFont = font;
        authorFont.setPointSize(authorFont.pointSize() - 1);
        authorFont.setBold(true);
        QFontMetrics authorFm(authorFont);
        layout.authorHeight = std::max(authorIconSize(), authorFm.height()) + 4;
        currentY += layout.authorHeight;
    }

    layout.titleY = currentY;
    layout.titleHeight = 0;
    if (!embed.title.isEmpty()) {
        QFont titleFont = font;
        titleFont.setBold(true);
        QFontMetrics titleFm(titleFont);
        layout.titleHeight = titleFm.height() + 4;
        currentY += layout.titleHeight;
    }

    layout.descriptionY = currentY;
    layout.descriptionHeight = 0;
    if (!embed.description.isEmpty()) {
        QTextDocument descDoc;
        descDoc.setDefaultFont(font);
        descDoc.setTextWidth(contentWidth);
        descDoc.setPlainText(embed.description);
        layout.descriptionHeight = int(std::ceil(descDoc.size().height())) + 8;
        currentY += layout.descriptionHeight;
    }

    layout.fieldsY = currentY;
    layout.fieldsHeight = 0;
    if (!embed.fields.isEmpty()) {
        QFont fieldNameFont = font;
        fieldNameFont.setBold(true);
        QFontMetrics fieldNameFm(fieldNameFont);
        int fieldWidth = (contentWidth - 2 * fieldSpacing()) / 3;

        int fieldsInRow = 0;
        int maxRowHeight = 0;
        int fieldsHeight = 0;

        for (const auto &field : embed.fields) {
            QTextDocument valueDoc;
            valueDoc.setDefaultFont(font);
            valueDoc.setTextWidth(field.isInline ? fieldWidth : contentWidth);
            valueDoc.setPlainText(field.value);
            int valueHeight = int(std::ceil(valueDoc.size().height()));
            int fieldHeight = fieldNameFm.height() + 2 + valueHeight;

            if (field.isInline) {
                fieldsInRow++;
                maxRowHeight = std::max(maxRowHeight, fieldHeight);
                if (fieldsInRow >= 3) {
                    fieldsHeight += maxRowHeight + fieldSpacing();
                    fieldsInRow = 0;
                    maxRowHeight = 0;
                }
            } else {
                if (fieldsInRow > 0) {
                    fieldsHeight += maxRowHeight + fieldSpacing();
                    fieldsInRow = 0;
                    maxRowHeight = 0;
                }
                fieldsHeight += fieldHeight + fieldSpacing();
            }
        }
        if (fieldsInRow > 0)
            fieldsHeight += maxRowHeight + fieldSpacing();

        layout.fieldsHeight = fieldsHeight;
        currentY += fieldsHeight;
    }

    layout.imagesY = currentY;
    layout.imagesHeight = 0;
    if (!embed.images.isEmpty()) {
        if (embed.images.size() == 1) {
            const auto &img = embed.images[0];
            if (!img.pixmap.isNull()) {
                QSize actualSize = img.pixmap.size().scaled(img.displaySize, Qt::KeepAspectRatio);
                layout.imagesHeight = actualSize.height();
            }
        } else {
            AttachmentGridLayout grid = calculateAttachmentGrid(embed.images.size(), contentWidth);
            layout.imagesHeight = grid.totalHeight;
        }
        currentY += layout.imagesHeight;
    } else if (!embed.videoThumbnail.isNull() && embed.thumbnail.isNull()) {
        QSize actualSize =
                embed.videoThumbnail.size().scaled(embed.videoThumbnailSize, Qt::KeepAspectRatio);
        layout.imagesHeight = actualSize.height();
        currentY += layout.imagesHeight;
    }

    layout.footerY = currentY;
    layout.footerHeight = 0;
    if (!embed.footerText.isEmpty()) {
        QFont footerFont = font;
        footerFont.setPointSize(footerFont.pointSize() - 2);
        QFontMetrics footerFm(footerFont);
        layout.footerHeight = std::max(footerIconSize(), footerFm.height()) + 4;
        currentY += layout.footerHeight;
    }

    if (layout.hasThumbnail)
        currentY = std::max(currentY, embedPadding() * 2 + thumbnailSize());

    layout.totalHeight = currentY;
    layout.thumbnailY = embedPadding();

    layout.embedRect = QRect(0, top, embedWidth, layout.totalHeight);
    layout.contentRect = QRect(embedBorderWidth() + embedPadding(), embedPadding(), contentWidth,
                               layout.totalHeight - embedPadding());

    return layout;
}

int calculateAttachmentsHeight(const QList<AttachmentData> &attachments, int textWidth)
{
    if (attachments.isEmpty())
        return 0;

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

    int totalHeight = 0;

    if (imageCount == 1) {
        totalHeight += firstImageSize.height() + padding();
    } else if (imageCount > 1) {
        AttachmentGridLayout grid = calculateAttachmentGrid(imageCount, textWidth);
        totalHeight += grid.totalHeight + padding();
    }

    totalHeight += fileCount * (fileAttachmentHeight() + padding());

    return totalHeight;
}

int calculateEmbedsHeight(const QList<EmbedData> &embeds, const QFont &font, int textWidth)
{
    if (embeds.isEmpty())
        return 0;

    int totalHeight = 0;
    for (const auto &embed : embeds) {
        EmbedLayout layout = calculateEmbedLayout(embed, font, textWidth, 0);
        totalHeight += layout.totalHeight + padding();
    }
    return totalHeight;
}

MessageLayout calculateMessageLayout(const LayoutContext &ctx)
{
    MessageLayout layout = {};

    QFontMetrics fm(ctx.font);

    QRect rowRect(0, ctx.rowTop, ctx.rowWidth, 10000);

    layout.showHeader = ctx.showHeader;
    layout.hasSeparator = ctx.hasSeparator;

    layout.separatorRect = ctx.hasSeparator ? dateSeparatorRectForRow(rowRect) : QRect();
    layout.avatarRect = avatarRectForRow(rowRect, ctx.hasSeparator);
    layout.headerRect = headerRectForRow(rowRect, fm, ctx.hasSeparator);

    int textLeft = padding() + avatarSize() + padding();
    int textWidth = ctx.rowWidth - textLeft - padding();
    if (textWidth < 10)
        textWidth = 10;

    QTextDocument doc;
    setupDocument(doc, ctx.htmlContent, ctx.font, textWidth);
    layout.textHeight = int(std::ceil(doc.size().height()));

    int textTop = ctx.rowTop + (ctx.hasSeparator ? separatorHeight() : 0);
    if (ctx.showHeader)
        textTop += padding() + fm.height();
    else
        textTop += 1;

    layout.textRect = QRect(textLeft, textTop, textWidth, layout.textHeight);

    int totalHeight = 0;
    if (ctx.showHeader) {
        int contentHeight = padding() + fm.height() + layout.textHeight + padding();
        int minHeight = padding() + avatarSize() + padding();
        totalHeight = std::max(contentHeight, minHeight);
    } else {
        totalHeight = layout.textHeight + padding() + 1;
    }

    if (ctx.hasSeparator)
        totalHeight += separatorHeight();

    layout.attachmentsTop = textTop + layout.textHeight + padding();
    layout.attachmentsTotalHeight = 0;

    if (!ctx.attachments.isEmpty()) {
        int imageIndex = 0;
        int fileIndex = 0;

        QList<AttachmentData> images;
        for (int i = 0; i < ctx.attachments.size(); ++i) {
            if (ctx.attachments[i].isImage)
                images.append(ctx.attachments[i]);
        }

        if (!images.isEmpty()) {
            layout.imageGrid = calculateAttachmentGrid(images.size(), textWidth);

            for (int i = 0; i < ctx.attachments.size(); ++i) {
                if (ctx.attachments[i].isImage) {
                    AttachmentLayout attLayout;
                    attLayout.index = i;
                    attLayout.isImage = true;

                    if (images.size() == 1) {
                        attLayout.rect = QRect(textLeft, layout.attachmentsTop,
                                               images[0].displaySize.width(),
                                               images[0].displaySize.height());
                    } else if (imageIndex < layout.imageGrid.cells.size()) {
                        attLayout.rect = layout.imageGrid.cells[imageIndex].rect.translated(
                                textLeft, layout.attachmentsTop);
                    }
                    layout.imageLayouts.append(attLayout);
                    imageIndex++;
                }
            }

            if (images.size() == 1)
                layout.attachmentsTotalHeight += images[0].displaySize.height() + padding();
            else
                layout.attachmentsTotalHeight += layout.imageGrid.totalHeight + padding();
        }

        int currentFileTop = layout.attachmentsTop + layout.attachmentsTotalHeight;
        int fileWidth = std::min(textWidth, maxAttachmentWidth());

        for (int i = 0; i < ctx.attachments.size(); ++i) {
            if (!ctx.attachments[i].isImage) {
                AttachmentLayout attLayout;
                attLayout.index = i;
                attLayout.isImage = false;
                attLayout.rect = QRect(textLeft, currentFileTop, fileWidth, fileAttachmentHeight());
                layout.fileLayouts.append(attLayout);
                currentFileTop += fileAttachmentHeight() + padding();
                layout.attachmentsTotalHeight += fileAttachmentHeight() + padding();
                fileIndex++;
            }
        }

        totalHeight += layout.attachmentsTotalHeight;
    }

    layout.embedsTop = layout.attachmentsTop + layout.attachmentsTotalHeight;
    layout.embedsTotalHeight = 0;

    if (!ctx.embeds.isEmpty()) {
        int embedTop = layout.embedsTop;
        for (const auto &embed : ctx.embeds) {
            EmbedLayout embedLayout = calculateEmbedLayout(embed, ctx.font, textWidth, embedTop);
            embedLayout.embedRect.moveLeft(textLeft);
            layout.embedLayouts.append(embedLayout);
            embedTop += embedLayout.totalHeight + padding();
            layout.embedsTotalHeight += embedLayout.totalHeight + padding();
        }
        totalHeight += layout.embedsTotalHeight;
    }

    layout.totalHeight = totalHeight;
    layout.rowRect = QRect(0, ctx.rowTop, ctx.rowWidth, totalHeight);

    return layout;
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

void drawCroppedPixmap(QPainter *painter, const QRect &targetRect, const QPixmap &pixmap)
{
    if (pixmap.isNull())
        return;

    QSize pixSize = pixmap.size() / pixmap.devicePixelRatio();
    QRect sourceRect;

    qreal targetAspect = qreal(targetRect.width()) / targetRect.height();
    qreal pixAspect = qreal(pixSize.width()) / pixSize.height();

    if (pixAspect > targetAspect) {
        int cropWidth = qRound(pixSize.height() * targetAspect);
        int cropX = (pixSize.width() - cropWidth) / 2;
        sourceRect = QRect(cropX, 0, cropWidth, pixSize.height());
    } else {
        int cropHeight = qRound(pixSize.width() / targetAspect);
        int cropY = (pixSize.height() - cropHeight) / 2;
        sourceRect = QRect(0, cropY, pixSize.width(), cropHeight);
    }

    qreal dpr = pixmap.devicePixelRatio();
    QRect physicalSourceRect(qRound(sourceRect.x() * dpr), qRound(sourceRect.y() * dpr),
                             qRound(sourceRect.width() * dpr), qRound(sourceRect.height() * dpr));

    painter->drawPixmap(targetRect, pixmap, physicalSourceRect);
}

int hitTestCharIndex(QAbstractItemView *view, const QModelIndex &index, const QPoint &viewportPos)
{
    if (!index.isValid() || !view)
        return -1;

    const bool showHeader = index.data(ChatModel::ShowHeaderRole).toBool();
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

std::optional<AttachmentData> getAttachmentAt(const QAbstractItemView *view,
                                              const QModelIndex &index, const QPoint &mousePos)
{
    if (!index.isValid() || !view)
        return std::nullopt;

    QList<AttachmentData> attachments =
            index.data(ChatModel::AttachmentsRole).value<QList<AttachmentData>>();

    if (attachments.isEmpty())
        return std::nullopt;

    LayoutContext ctx;
    ctx.font = view->font();
    ctx.rowWidth = view->visualRect(index).width();
    ctx.rowTop = view->visualRect(index).top();
    ctx.showHeader = index.data(ChatModel::ShowHeaderRole).toBool();
    ctx.hasSeparator = index.data(ChatModel::DateSeparatorRole).toBool();
    ctx.htmlContent = index.data(ChatModel::HtmlRole).toString();
    ctx.attachments = attachments;

    MessageLayout layout = calculateMessageLayout(ctx);

    for (const auto &imgLayout : layout.imageLayouts) {
        if (imgLayout.rect.contains(mousePos))
            return attachments[imgLayout.index];
    }

    for (const auto &fileLayout : layout.fileLayouts) {
        if (fileLayout.rect.contains(mousePos))
            return attachments[fileLayout.index];
    }

    return std::nullopt;
}

std::optional<EmbedHitResult> getEmbedAt(const QAbstractItemView *view, const QModelIndex &index,
                                         const QPoint &mousePos)
{
    if (!index.isValid() || !view)
        return std::nullopt;

    QList<EmbedData> embeds = index.data(ChatModel::EmbedsRole).value<QList<EmbedData>>();
    if (embeds.isEmpty())
        return std::nullopt;

    LayoutContext ctx;
    ctx.font = view->font();
    QRect rowRect = view->visualRect(index);
    ctx.rowWidth = rowRect.width();
    ctx.rowTop = rowRect.top();
    ctx.showHeader = index.data(ChatModel::ShowHeaderRole).toBool();
    ctx.hasSeparator = index.data(ChatModel::DateSeparatorRole).toBool();
    ctx.htmlContent = index.data(ChatModel::HtmlRole).toString();
    ctx.attachments = index.data(ChatModel::AttachmentsRole).value<QList<AttachmentData>>();
    ctx.embeds = embeds;

    MessageLayout layout = calculateMessageLayout(ctx);

    for (int embedIndex = 0; embedIndex < layout.embedLayouts.size(); ++embedIndex) {
        const auto &embedLayout = layout.embedLayouts[embedIndex];
        const auto &embed = embeds[embedIndex];

        if (!embedLayout.embedRect.contains(mousePos))
            continue;

        int contentLeft = embedLayout.embedRect.left() + embedBorderWidth() + embedPadding();
        int contentTop = embedLayout.embedRect.top() + embedPadding();

        if (embedLayout.hasThumbnail) {
            QSize thumbSize =
                    !embed.thumbnail.isNull() ? embed.thumbnailSize : embed.videoThumbnailSize;
            int thumbX = contentLeft + embedLayout.contentWidth + embedPadding();
            QRect thumbRect(thumbX, contentTop + embedLayout.thumbnailY, thumbSize.width(),
                            thumbSize.height());
            if (thumbRect.contains(mousePos)) {
                EmbedHitResult result;
                result.embedIndex = embedIndex;
                result.hitType = !embed.thumbnail.isNull() ? EmbedHitType::Image
                                                           : EmbedHitType::VideoThumbnail;
                result.image = !embed.thumbnail.isNull() ? embed.thumbnail : embed.videoThumbnail;
                result.imageSize = thumbSize;
                result.url = embed.thumbnailUrl.toString();
                return result;
            }
        }

        if (!embed.authorName.isEmpty() && embedLayout.authorHeight > 0) {
            QRect authorRect(contentLeft, contentTop + embedLayout.authorY,
                             embedLayout.contentWidth, embedLayout.authorHeight);
            if (authorRect.contains(mousePos) && !embed.authorUrl.isEmpty()) {
                EmbedHitResult result;
                result.embedIndex = embedIndex;
                result.hitType = EmbedHitType::Author;
                result.url = embed.authorUrl;
                return result;
            }
        }

        if (!embed.title.isEmpty() && embedLayout.titleHeight > 0) {
            QRect titleRect(contentLeft, contentTop + embedLayout.titleY, embedLayout.contentWidth,
                            embedLayout.titleHeight);
            if (titleRect.contains(mousePos) && !embed.url.isEmpty()) {
                EmbedHitResult result;
                result.embedIndex = embedIndex;
                result.hitType = EmbedHitType::Title;
                result.url = embed.url;
                return result;
            }
        }

        if (!embed.images.isEmpty() && embedLayout.imagesHeight > 0) {
            int imagesTop = contentTop + embedLayout.imagesY;

            if (embed.images.size() == 1) {
                const auto &img = embed.images[0];
                if (!img.pixmap.isNull()) {
                    QSize actualSize =
                            img.pixmap.size().scaled(img.displaySize, Qt::KeepAspectRatio);
                    QRect imageRect(contentLeft, imagesTop, actualSize.width(),
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
                        calculateAttachmentGrid(embed.images.size(), embedLayout.contentWidth);
                for (const auto &cell : grid.cells) {
                    if (cell.attachmentIndex >= embed.images.size())
                        continue;

                    QRect imgRect = cell.rect.translated(contentLeft, imagesTop);
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
            int videoTop = contentTop + embedLayout.imagesY;
            QSize actualSize = embed.videoThumbnail.size().scaled(embed.videoThumbnailSize,
                                                                  Qt::KeepAspectRatio);
            QRect videoRect(contentLeft, videoTop, actualSize.width(), actualSize.height());
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

    return std::nullopt;
}

} // namespace ChatLayout
} // namespace UI
} // namespace Acheron
