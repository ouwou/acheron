#include "ChatLayout.hpp"
#include <QPainter>
#include <QGraphicsBlurEffect>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>

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
    return QRect(rowRect.left() + padding(), rowRect.top() + blockTopPadding() + topOffset, avatarSize(),
                 avatarSize());
}

QRect headerRectForRow(const QRect &rowRect, const QFontMetrics &fm, bool hasSeparator)
{
    int topOffset = hasSeparator ? separatorHeight() : 0;
    int left = rowRect.left() + padding() + avatarSize() + padding();
    int width = rowRect.right() - left - padding();
    int capDrop = fm.ascent() - fm.capHeight();
    return QRect(left, rowRect.top() + blockTopPadding() + topOffset - capDrop, width, fm.height());
}

QRect textRectForRow(const QRect &rowRect, bool showHeader, const QFontMetrics &fm,
                     bool hasSeparator)
{
    int topOffset = hasSeparator ? separatorHeight() : 0;
    int left = rowRect.left() + padding() + avatarSize() + padding();
    int width = rowRect.right() - left - padding();
    if (width < 10)
        width = 10;

    int capDrop = fm.ascent() - fm.capHeight();
    int top = rowRect.top() + topOffset;
    if (showHeader)
        top += blockTopPadding() - capDrop + fm.height();
    else
        top += 0;

    int height = rowRect.bottom() - top - padding() + 1;
    if (height < 0)
        height = 0;

    return QRect(left, top, width, height);
}

void setupDocument(QTextDocument &doc, const QString &htmlContent, const QFont &font, int textWidth)
{
    QString wrapped = QString("<div style=\"white-space: pre-wrap;\">%1</div>")
                              .arg(htmlContent);

    doc.setDefaultFont(font);
    doc.setHtml(wrapped);
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

EmbedLayout calculateEmbedLayout(const EmbedData &embed, const QFont &font, int maxWidth, int left,
                                 int top, const ChatModel *model, Core::Snowflake messageId,
                                 int embedIndex)
{
    EmbedLayout layout = {};

    int embedWidth = std::min(maxWidth, embedMaxWidth());
    int contentWidth = embedWidth - embedBorderWidth() - embedPadding() * 2;
    int contentLeft = left + embedBorderWidth() + embedPadding();

    if (embed.type == EmbedType::Gifv) {
        layout.hasThumbnail = false;
        layout.contentWidth = contentWidth;
        int currentY = 0;

        int imagesHeight = 0;
        if (!embed.thumbnail.isNull()) {
            QSize actualSize =
                    embed.thumbnail.size().scaled(embed.thumbnailSize, Qt::KeepAspectRatio);
            imagesHeight = actualSize.height();
            layout.imagesRect =
                    QRect(left, top + currentY, actualSize.width(), actualSize.height());
            currentY += imagesHeight;
        }

        QFont gifFont = font;
        gifFont.setPointSize(gifFont.pointSize() - 2);
        QFontMetrics gifFm(gifFont);
        int gifLabelHeight = gifFm.height() + 4;
        currentY += gifLabelHeight;

        layout.totalHeight = currentY;
        layout.embedRect = QRect(left, top, embedWidth, layout.totalHeight);
        layout.contentRect = QRect(contentLeft, top, contentWidth, layout.totalHeight);

        return layout;
    }

    if (embed.type == EmbedType::Image) {
        layout.hasThumbnail = false;
        layout.contentWidth = contentWidth;
        int currentY = 0;

        if (!embed.thumbnail.isNull()) {
            QSize actualSize =
                    embed.thumbnail.size().scaled(embed.thumbnailSize, Qt::KeepAspectRatio);
            layout.imagesRect =
                    QRect(left, top + currentY, actualSize.width(), actualSize.height());
            currentY += actualSize.height();
        }

        layout.totalHeight = currentY;
        layout.embedRect = QRect(left, top, embedWidth, layout.totalHeight);
        layout.contentRect = QRect(contentLeft, top, contentWidth, layout.totalHeight);

        return layout;
    }

    layout.hasThumbnail =
            !embed.thumbnail.isNull() || (!embed.videoThumbnail.isNull() && embed.images.isEmpty());
    if (layout.hasThumbnail)
        contentWidth -= (thumbnailSize() + embedPadding());

    layout.contentWidth = contentWidth;

    int contentTop = top + embedPadding();
    int currentY = 0;

    if (!embed.providerName.isEmpty()) {
        QFont providerFont = font;
        providerFont.setPointSize(providerFont.pointSize() - 2);
        QFontMetrics providerFm(providerFont);
        int providerHeight = providerFm.height() + 4;
        layout.providerRect =
                QRect(contentLeft, contentTop + currentY, contentWidth, providerHeight);
        currentY += providerHeight;
    }

    if (!embed.authorName.isEmpty()) {
        QFont authorFont = font;
        authorFont.setPointSize(authorFont.pointSize() - 1);
        authorFont.setBold(true);
        QFontMetrics authorFm(authorFont);
        int authorHeight = std::max(authorIconSize(), authorFm.height()) + 4;
        layout.authorRect = QRect(contentLeft, contentTop + currentY, contentWidth, authorHeight);
        currentY += authorHeight;
    }

    if (!embed.title.isEmpty()) {
        QFont titleFont = font;
        titleFont.setBold(true);
        QString titleHtml = !embed.titleParsed.isEmpty() ? embed.titleParsed : embed.title;
        int titleHeight;
        QTextDocument *cached = model ? model->getCachedDocument(embedTitleDocKey(messageId, embedIndex))
                                      : nullptr;
        if (cached) {
            if (int(cached->textWidth()) != contentWidth)
                cached->setTextWidth(contentWidth);
            titleHeight = int(std::ceil(cached->size().height())) + 4;
        } else {
            QTextDocument titleDoc;
            titleDoc.setDefaultFont(titleFont);
            titleDoc.setTextWidth(contentWidth);
            titleDoc.setHtml(titleHtml);
            titleHeight = int(std::ceil(titleDoc.size().height())) + 4;
        }
        layout.titleRect = QRect(contentLeft, contentTop + currentY, contentWidth, titleHeight);
        currentY += titleHeight;
    }

    if (!embed.description.isEmpty()) {
        QString descHtml = !embed.descriptionParsed.isEmpty() ? embed.descriptionParsed : embed.description;
        int descriptionHeight;
        QTextDocument *cached = model ? model->getCachedDocument(embedDescDocKey(messageId, embedIndex))
                                      : nullptr;
        if (cached) {
            if (int(cached->textWidth()) != contentWidth)
                cached->setTextWidth(contentWidth);
            descriptionHeight = int(std::ceil(cached->size().height())) + 8;
        } else {
            QTextDocument descDoc;
            descDoc.setDefaultFont(font);
            descDoc.setTextWidth(contentWidth);
            descDoc.setHtml(descHtml);
            descriptionHeight = int(std::ceil(descDoc.size().height())) + 8;
        }
        layout.descriptionRect =
                QRect(contentLeft, contentTop + currentY, contentWidth, descriptionHeight);
        currentY += descriptionHeight;
    }

    if (!embed.fields.isEmpty()) {
        QFont fieldNameFont = font;
        fieldNameFont.setBold(true);
        QFontMetrics fieldNameFm(fieldNameFont);
        int fieldWidth = (contentWidth - 2 * fieldSpacing()) / 3;

        int fieldsStartY = currentY;
        int fieldX = 0;
        int fieldsInRow = 0;
        int rowStartY = currentY;
        int maxRowHeight = 0;

        for (int i = 0; i < embed.fields.size(); ++i) {
            const auto &field = embed.fields[i];
            int fldTextWidth = field.isInline ? fieldWidth : contentWidth;

            QString nameHtml = !field.nameParsed.isEmpty() ? field.nameParsed : field.name;
            int nameHeight;
            QTextDocument *cachedName = model
                                                ? model->getCachedDocument(embedFieldNameDocKey(messageId, embedIndex, i))
                                                : nullptr;
            if (cachedName) {
                if (int(cachedName->textWidth()) != fldTextWidth)
                    cachedName->setTextWidth(fldTextWidth);
                nameHeight = int(std::ceil(cachedName->size().height()));
            } else {
                QTextDocument nameDoc;
                nameDoc.setDefaultFont(fieldNameFont);
                nameDoc.setTextWidth(fldTextWidth);
                nameDoc.setHtml(nameHtml);
                nameHeight = int(std::ceil(nameDoc.size().height()));
            }

            QString valueHtml = !field.valueParsed.isEmpty() ? field.valueParsed : field.value;
            int valueHeight;
            QTextDocument *cachedValue = model ? model->getCachedDocument(embedFieldValueDocKey(messageId, embedIndex, i))
                                               : nullptr;
            if (cachedValue) {
                if (int(cachedValue->textWidth()) != fldTextWidth)
                    cachedValue->setTextWidth(fldTextWidth);
                valueHeight = int(std::ceil(cachedValue->size().height()));
            } else {
                QTextDocument valueDoc;
                valueDoc.setDefaultFont(font);
                valueDoc.setTextWidth(fldTextWidth);
                valueDoc.setHtml(valueHtml);
                valueHeight = int(std::ceil(valueDoc.size().height()));
            }
            int fieldHeight = nameHeight + 2 + valueHeight;

            EmbedFieldLayout fieldLayout;
            fieldLayout.fieldIndex = i;

            if (!field.isInline) {
                if (fieldsInRow > 0) {
                    currentY = rowStartY + maxRowHeight + fieldSpacing();
                    fieldX = 0;
                    fieldsInRow = 0;
                    maxRowHeight = 0;
                    rowStartY = currentY;
                }

                fieldLayout.nameRect =
                        QRect(contentLeft, contentTop + currentY, contentWidth, nameHeight);
                fieldLayout.valueRect = QRect(contentLeft, contentTop + currentY + nameHeight + 2,
                                              contentWidth, valueHeight);
                layout.fieldLayouts.append(fieldLayout);

                currentY += fieldHeight + fieldSpacing();
                rowStartY = currentY;
            } else {
                if (fieldsInRow >= 3) {
                    currentY = rowStartY + maxRowHeight + fieldSpacing();
                    fieldX = 0;
                    fieldsInRow = 0;
                    maxRowHeight = 0;
                    rowStartY = currentY;
                }

                int xPos = contentLeft + fieldX;
                fieldLayout.nameRect = QRect(xPos, contentTop + currentY, fieldWidth, nameHeight);
                fieldLayout.valueRect = QRect(xPos, contentTop + currentY + nameHeight + 2,
                                              fieldWidth, valueHeight);
                layout.fieldLayouts.append(fieldLayout);

                maxRowHeight = std::max(maxRowHeight, fieldHeight);
                fieldX += fieldWidth + fieldSpacing();
                fieldsInRow++;
            }
        }

        if (fieldsInRow > 0)
            currentY = rowStartY + maxRowHeight + fieldSpacing();
    }

    if (!embed.images.isEmpty()) {
        int imagesTop = contentTop + currentY;

        if (embed.images.size() == 1) {
            const auto &img = embed.images[0];
            if (!img.pixmap.isNull()) {
                QSize actualSize = img.pixmap.size().scaled(img.displaySize, Qt::KeepAspectRatio);
                layout.imagesRect =
                        QRect(contentLeft, imagesTop, actualSize.width(), actualSize.height());
                layout.imageLayouts.append({ 0, layout.imagesRect });
                currentY += actualSize.height();
            }
        } else {
            AttachmentGridLayout grid = calculateAttachmentGrid(embed.images.size(), contentWidth);
            layout.imagesRect = QRect(contentLeft, imagesTop, contentWidth, grid.totalHeight);
            for (const auto &cell : grid.cells) {
                QRect imgRect = cell.rect.translated(contentLeft, imagesTop);
                layout.imageLayouts.append({ cell.attachmentIndex, imgRect });
            }
            currentY += grid.totalHeight;
        }
    } else if (!embed.videoThumbnail.isNull() && embed.thumbnail.isNull()) {
        int imagesTop = contentTop + currentY;
        QSize actualSize =
                embed.videoThumbnail.size().scaled(embed.videoThumbnailSize, Qt::KeepAspectRatio);
        layout.imagesRect = QRect(contentLeft, imagesTop, actualSize.width(), actualSize.height());
        currentY += actualSize.height();
    }

    if (!embed.footerText.isEmpty()) {
        QFont footerFont = font;
        footerFont.setPointSize(footerFont.pointSize() - 2);
        QFontMetrics footerFm(footerFont);
        int footerHeight = std::max(footerIconSize(), footerFm.height()) + 4;
        layout.footerRect = QRect(contentLeft, contentTop + currentY, contentWidth, footerHeight);
        currentY += footerHeight;
    }

    int totalContentHeight = embedPadding() + currentY;
    if (layout.hasThumbnail) {
        int thumbTop = contentTop;
        int thumbX = contentLeft + contentWidth + embedPadding();
        QSize thumbSize =
                !embed.thumbnail.isNull() ? embed.thumbnailSize : embed.videoThumbnailSize;
        layout.thumbnailRect = QRect(thumbX, thumbTop, thumbSize.width(), thumbSize.height());
        totalContentHeight = std::max(totalContentHeight, embedPadding() * 2 + thumbnailSize());
    }

    layout.totalHeight = totalContentHeight;
    layout.embedRect = QRect(left, top, embedWidth, layout.totalHeight);
    layout.contentRect = QRect(contentLeft, contentTop, contentWidth, currentY);

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
        EmbedLayout layout = calculateEmbedLayout(embed, font, textWidth, 0, 0);
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
    layout.hasReply = ctx.replyData.state != ReplyData::State::None;

    layout.separatorRect = ctx.hasSeparator ? dateSeparatorRectForRow(rowRect) : QRect();

    int replyOffset = 0;
    if (layout.hasReply) {
        int replyTop = ctx.rowTop + padding() + (ctx.hasSeparator ? separatorHeight() : 0);
        int replyLeft = padding() + avatarSize() + padding();
        int replyWidth = ctx.rowWidth - replyLeft - padding();
        layout.replyRect = QRect(replyLeft, replyTop, replyWidth, replyBarHeight());
        replyOffset = padding() + replyBarHeight();
    }

    int textLeft = padding() + avatarSize() + padding();
    int textWidth = ctx.rowWidth - textLeft - padding();
    if (textWidth < 10)
        textWidth = 10;

    int separatorOffset = ctx.hasSeparator ? separatorHeight() : 0;
    int headerAreaTop = ctx.rowTop + separatorOffset + replyOffset;

    int capDrop = fm.ascent() - fm.capHeight();
    if (layout.hasReply) {
        // Reply messages: no extra top padding, reply bar provides the visual gap
        layout.avatarRect = QRect(padding(), headerAreaTop, avatarSize(), avatarSize());
        layout.headerRect = QRect(textLeft, headerAreaTop - capDrop, textWidth, fm.height());
    } else {
        QRect baseRowRect(0, ctx.rowTop, ctx.rowWidth, 10000);
        layout.avatarRect = avatarRectForRow(baseRowRect, ctx.hasSeparator);
        layout.headerRect = headerRectForRow(baseRowRect, fm, ctx.hasSeparator);
    }

    if (!ctx.htmlContent.isEmpty()) {
        QTextDocument *cached = ctx.model ? ctx.model->getCachedDocument(bodyDocKey(ctx.messageId))
                                          : nullptr;
        if (cached) {
            if (int(cached->textWidth()) != textWidth)
                cached->setTextWidth(textWidth);
            layout.textHeight = int(std::ceil(cached->size().height()));
        } else {
            QFont docFont = ctx.font;
            if (ctx.isSystemMessage)
                docFont.setItalic(true);
            QTextDocument doc;
            setupDocument(doc, ctx.htmlContent, docFont, textWidth);
            layout.textHeight = int(std::ceil(doc.size().height()));
        }
    }

    int textTop;
    if (layout.hasReply) {
        textTop = headerAreaTop - capDrop;
        if (ctx.showHeader)
            textTop += fm.height();
    } else {
        textTop = ctx.rowTop + separatorOffset;
        if (ctx.showHeader)
            textTop += blockTopPadding() - capDrop + fm.height();
        else
            textTop += 0;
    }

    layout.textRect = QRect(textLeft, textTop, textWidth, layout.textHeight);

    int totalHeight = 0;
    if (layout.hasReply) {
        totalHeight = padding() + replyBarHeight() - capDrop + fm.height() + layout.textHeight + padding() / 2;
    } else if (ctx.showHeader) {
        int contentHeight = blockTopPadding() - capDrop + fm.height() + layout.textHeight + padding() / 2;
        totalHeight = contentHeight;
    } else {
        totalHeight = layout.textHeight + padding() / 2;
    }

    if (ctx.hasSeparator)
        totalHeight += separatorHeight();

    layout.attachmentsTop = textTop + layout.textHeight;
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
                layout.attachmentsTotalHeight += images[0].displaySize.height();
            else
                layout.attachmentsTotalHeight += layout.imageGrid.totalHeight;
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
        for (int ei = 0; ei < ctx.embeds.size(); ++ei) {
            const auto &embed = ctx.embeds[ei];
            EmbedLayout embedLayout = calculateEmbedLayout(embed, ctx.font, textWidth, textLeft,
                                                           embedTop, ctx.model, ctx.messageId, ei);
            layout.embedLayouts.append(embedLayout);
            embedTop += embedLayout.totalHeight + padding();
            layout.embedsTotalHeight += embedLayout.totalHeight + padding();
        }
        totalHeight += layout.embedsTotalHeight;
    }

    layout.reactionsTop = layout.embedsTop + layout.embedsTotalHeight;
    layout.reactionsTotalHeight = 0;

    if (!ctx.reactions.isEmpty()) {
        QFontMetrics reactionFm(ctx.font);
        int currentX = textLeft;
        int currentY = layout.reactionsTop + reactionTopMargin();
        int rowHeight = reactionPillHeight();
        int maxX = textLeft + textWidth;

        for (int i = 0; i < ctx.reactions.size(); ++i) {
            const auto &reaction = ctx.reactions[i];

            int countTextWidth = reactionFm.horizontalAdvance(QString::number(reaction.count));
            int pillWidth = reactionPillPadding() + reactionEmojiSize() + 4 + countTextWidth + reactionPillPadding();

            // wrap to next row if needed
            if (currentX + pillWidth > maxX && currentX > textLeft) {
                currentX = textLeft;
                currentY += rowHeight + reactionRowSpacing();
            }

            QRect pillRect(currentX, currentY, pillWidth, rowHeight);
            QRect emojiRect(currentX + reactionPillPadding(),
                            currentY + (rowHeight - reactionEmojiSize()) / 2,
                            reactionEmojiSize(), reactionEmojiSize());
            QRect countRect(emojiRect.right() + 4,
                            currentY,
                            countTextWidth,
                            rowHeight);

            ReactionLayout rl;
            rl.reactionIndex = i;
            rl.pillRect = pillRect;
            rl.emojiRect = emojiRect;
            rl.countRect = countRect;
            layout.reactionLayouts.append(rl);

            currentX += pillWidth + reactionSpacing();
        }

        layout.reactionsTotalHeight = reactionTopMargin() + (currentY - layout.reactionsTop - reactionTopMargin()) + rowHeight;
        totalHeight += layout.reactionsTotalHeight;
    }

    // last ditch minimum size but only with headers
    if (ctx.showHeader) {
        int minHeight = padding() + avatarSize() + padding();
        if (totalHeight < minHeight)
            totalHeight = minHeight;
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
    ReplyData reply = index.data(ChatModel::ReplyDataRole).value<ReplyData>();
    bool hasReply = reply.state != ReplyData::State::None;
    int replyOffset = hasReply ? replyBarHeight() - padding() : 0;

    QFont docFont = getFontForIndex(view, index);
    QFontMetrics fm(docFont);

    QRect rowRect = view->visualRect(index);
    QRect textRect = textRectForRow(rowRect, showHeader, fm, hasSeparator);
    textRect.translate(0, replyOffset);

    const auto *chatModel = qobject_cast<const ChatModel *>(index.model());
    Snowflake msgId = index.data(ChatModel::MessageIdRole).toULongLong();
    QTextDocument *doc = chatModel->getCachedDocument(bodyDocKey(msgId));
    QTextDocument localDoc;
    if (!doc) {
        setupDocument(localDoc, html, docFont, textRect.width());
        doc = &localDoc;
    } else if (int(doc->textWidth()) != textRect.width()) {
        doc->setTextWidth(textRect.width());
    }

    QPointF local = viewportPos - textRect.topLeft();

    if (local.y() < 0 || local.y() > doc->size().height())
        return -1;

    return doc->documentLayout()->hitTest(local, Qt::ExactHit);
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
    ReplyData reply = index.data(ChatModel::ReplyDataRole).value<ReplyData>();
    bool hasReply = reply.state != ReplyData::State::None;
    int replyOffset = hasReply ? replyBarHeight() - padding() : 0;
    QFont font = view->font();
    QFontMetrics fm(font);

    QRect textRect = textRectForRow(rowRect, showHeader, fm, hasSeparator);
    textRect.translate(0, replyOffset);

    if (!textRect.contains(mousePos))
        return {};

    const auto *chatModel = qobject_cast<const ChatModel *>(index.model());
    Snowflake msgId = index.data(ChatModel::MessageIdRole).toULongLong();
    QTextDocument *doc = chatModel->getCachedDocument(bodyDocKey(msgId));
    QTextDocument localDoc;
    if (!doc) {
        setupDocument(localDoc, html, font, textRect.width());
        doc = &localDoc;
    } else if (int(doc->textWidth()) != textRect.width()) {
        doc->setTextWidth(textRect.width());
    }

    QPointF localPos = mousePos - textRect.topLeft();

    return doc->documentLayout()->anchorAt(localPos);
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
    ctx.replyData = index.data(ChatModel::ReplyDataRole).value<ReplyData>();
    ctx.model = qobject_cast<const ChatModel *>(index.model());
    ctx.messageId = index.data(ChatModel::MessageIdRole).toULongLong();

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

    const auto *chatModel = qobject_cast<const ChatModel *>(index.model());
    Snowflake msgId = index.data(ChatModel::MessageIdRole).toULongLong();

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
    ctx.replyData = index.data(ChatModel::ReplyDataRole).value<ReplyData>();
    ctx.model = chatModel;
    ctx.messageId = msgId;

    MessageLayout layout = calculateMessageLayout(ctx);

    for (int embedIndex = 0; embedIndex < layout.embedLayouts.size(); ++embedIndex) {
        const auto &embedLayout = layout.embedLayouts[embedIndex];
        const auto &embed = embeds[embedIndex];

        if (!embedLayout.embedRect.contains(mousePos))
            continue;

        if (embedLayout.hasThumbnail && !embedLayout.thumbnailRect.isNull()) {
            if (embedLayout.thumbnailRect.contains(mousePos)) {
                EmbedHitResult result;
                result.embedIndex = embedIndex;
                result.hitType = !embed.thumbnail.isNull() ? EmbedHitType::Image
                                                           : EmbedHitType::VideoThumbnail;
                result.image = !embed.thumbnail.isNull() ? embed.thumbnail : embed.videoThumbnail;
                result.imageSize = embedLayout.thumbnailRect.size();
                result.url = embed.thumbnailUrl.toString();
                return result;
            }
        }

        if (!embed.authorName.isEmpty() && !embedLayout.authorRect.isNull()) {
            if (embedLayout.authorRect.contains(mousePos) && !embed.authorUrl.isEmpty()) {
                EmbedHitResult result;
                result.embedIndex = embedIndex;
                result.hitType = EmbedHitType::Author;
                result.url = embed.authorUrl;
                return result;
            }
        }

        if (!embed.title.isEmpty() && !embedLayout.titleRect.isNull()) {
            if (embedLayout.titleRect.contains(mousePos)) {
                if (!embed.titleParsed.isEmpty()) {
                    QFont titleFont = view->font();
                    titleFont.setBold(true);
                    QTextDocument *titleDoc = chatModel->getCachedDocument(embedTitleDocKey(msgId, embedIndex));
                    QTextDocument localTitleDoc;
                    if (!titleDoc) {
                        localTitleDoc.setDefaultFont(titleFont);
                        localTitleDoc.setTextWidth(embedLayout.titleRect.width());
                        localTitleDoc.setHtml(embed.titleParsed);
                        titleDoc = &localTitleDoc;
                    }
                    QPointF localPos = mousePos - embedLayout.titleRect.topLeft();
                    QString linkUrl = titleDoc->documentLayout()->anchorAt(localPos);
                    if (!linkUrl.isEmpty()) {
                        EmbedHitResult result;
                        result.embedIndex = embedIndex;
                        result.hitType = EmbedHitType::Link;
                        result.url = linkUrl;
                        return result;
                    }
                }

                if (!embed.url.isEmpty()) {
                    EmbedHitResult result;
                    result.embedIndex = embedIndex;
                    result.hitType = EmbedHitType::Title;
                    result.url = embed.url;
                    return result;
                }
            }
        }

        if (!embed.images.isEmpty() && !embedLayout.imageLayouts.isEmpty()) {
            for (const auto &imgLayout : embedLayout.imageLayouts) {
                if (imgLayout.imageIndex >= embed.images.size())
                    continue;

                if (imgLayout.rect.contains(mousePos)) {
                    const auto &img = embed.images[imgLayout.imageIndex];
                    EmbedHitResult result;
                    result.embedIndex = embedIndex;
                    result.hitType = EmbedHitType::Image;
                    result.image = img.pixmap;
                    result.imageSize = imgLayout.rect.size();
                    result.url = img.url.toString();
                    return result;
                }
            }
        } else if (!embed.videoThumbnail.isNull() && embed.thumbnail.isNull()) {
            if (!embedLayout.imagesRect.isNull() && embedLayout.imagesRect.contains(mousePos)) {
                EmbedHitResult result;
                result.embedIndex = embedIndex;
                result.hitType = EmbedHitType::VideoThumbnail;
                result.image = embed.videoThumbnail;
                result.imageSize = embedLayout.imagesRect.size();
                result.url = embed.url;
                return result;
            }
        }

        if (!embed.description.isEmpty() && !embedLayout.descriptionRect.isNull() &&
            embedLayout.descriptionRect.contains(mousePos) && !embed.descriptionParsed.isEmpty()) {
            QTextDocument *descDoc = chatModel->getCachedDocument(embedDescDocKey(msgId, embedIndex));
            QTextDocument localDescDoc;
            if (!descDoc) {
                localDescDoc.setDefaultFont(view->font());
                localDescDoc.setTextWidth(embedLayout.descriptionRect.width());
                localDescDoc.setHtml(embed.descriptionParsed);
                descDoc = &localDescDoc;
            }
            QPointF localPos = mousePos - embedLayout.descriptionRect.topLeft();
            QString linkUrl = descDoc->documentLayout()->anchorAt(localPos);
            if (!linkUrl.isEmpty()) {
                EmbedHitResult result;
                result.embedIndex = embedIndex;
                result.hitType = EmbedHitType::Link;
                result.url = linkUrl;
                return result;
            }
        }

        for (const auto &fieldLayout : embedLayout.fieldLayouts) {
            if (fieldLayout.fieldIndex >= embed.fields.size())
                continue;

            const auto &field = embed.fields[fieldLayout.fieldIndex];

            if (fieldLayout.nameRect.contains(mousePos) && !field.nameParsed.isEmpty()) {
                QFont fieldNameFont = view->font();
                fieldNameFont.setBold(true);
                QTextDocument *nameDoc = chatModel->getCachedDocument(embedFieldNameDocKey(msgId, embedIndex, fieldLayout.fieldIndex));
                QTextDocument localNameDoc;
                if (!nameDoc) {
                    localNameDoc.setDefaultFont(fieldNameFont);
                    localNameDoc.setTextWidth(fieldLayout.nameRect.width());
                    localNameDoc.setHtml(field.nameParsed);
                    nameDoc = &localNameDoc;
                }
                QPointF localPos = mousePos - fieldLayout.nameRect.topLeft();
                QString linkUrl = nameDoc->documentLayout()->anchorAt(localPos);
                if (!linkUrl.isEmpty()) {
                    EmbedHitResult result;
                    result.embedIndex = embedIndex;
                    result.hitType = EmbedHitType::Link;
                    result.url = linkUrl;
                    return result;
                }
            }

            if (fieldLayout.valueRect.contains(mousePos) && !field.valueParsed.isEmpty()) {
                QTextDocument *valueDoc = chatModel->getCachedDocument(embedFieldValueDocKey(msgId, embedIndex, fieldLayout.fieldIndex));
                QTextDocument localValueDoc;
                if (!valueDoc) {
                    localValueDoc.setDefaultFont(view->font());
                    localValueDoc.setTextWidth(fieldLayout.valueRect.width());
                    localValueDoc.setHtml(field.valueParsed);
                    valueDoc = &localValueDoc;
                }
                QPointF localPos = mousePos - fieldLayout.valueRect.topLeft();
                QString linkUrl = valueDoc->documentLayout()->anchorAt(localPos);
                if (!linkUrl.isEmpty()) {
                    EmbedHitResult result;
                    result.embedIndex = embedIndex;
                    result.hitType = EmbedHitType::Link;
                    result.url = linkUrl;
                    return result;
                }
            }
        }
    }

    return std::nullopt;
}

std::optional<ReactionHitResult> getReactionAt(const QAbstractItemView *view,
                                               const QModelIndex &index, const QPoint &mousePos)
{
    if (!index.isValid() || !view)
        return std::nullopt;

    QList<ReactionData> reactions =
            index.data(ChatModel::ReactionsRole).value<QList<ReactionData>>();

    if (reactions.isEmpty())
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
    ctx.embeds = index.data(ChatModel::EmbedsRole).value<QList<EmbedData>>();
    ctx.reactions = reactions;
    ctx.replyData = index.data(ChatModel::ReplyDataRole).value<ReplyData>();
    ctx.model = qobject_cast<const ChatModel *>(index.model());
    ctx.messageId = index.data(ChatModel::MessageIdRole).toULongLong();

    MessageLayout layout = calculateMessageLayout(ctx);

    for (const auto &reactionLayout : layout.reactionLayouts) {
        if (reactionLayout.pillRect.contains(mousePos)) {
            ReactionHitResult result;
            result.reactionIndex = reactionLayout.reactionIndex;
            result.reaction = reactions[reactionLayout.reactionIndex];
            return result;
        }
    }

    return std::nullopt;
}

QPixmap createBlurredPixmap(const QPixmap &source, int blurRadius)
{
    if (source.isNull())
        return source;

    QGraphicsScene scene;
    QGraphicsPixmapItem item(source);

    QGraphicsBlurEffect *blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(blurRadius);
    blur->setBlurHints(QGraphicsBlurEffect::PerformanceHint);
    item.setGraphicsEffect(blur);

    scene.addItem(&item);

    QPixmap result(source.size());
    result.setDevicePixelRatio(source.devicePixelRatio());
    result.fill(Qt::transparent);

    QPainter painter(&result);
    scene.render(&painter);
    painter.end();

    return result;
}

} // namespace ChatLayout
} // namespace UI
} // namespace Acheron
