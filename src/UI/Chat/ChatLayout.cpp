#include "ChatLayout.hpp"

#include "Core/Theme/Fonts.hpp"
#include "Core/Theme/Icons.hpp"
#include "Core/Theme/Manager.hpp"
#include "Core/Theme/Tokens.hpp"

#include <QPainter>
#include <QGraphicsBlurEffect>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QTextBlock>
#include <QUrl>

namespace Acheron {
namespace UI {
namespace ChatLayout {

QRect dateSeparatorRectForRow(const QRect &rowRect)
{
    return QRect(rowRect.left(), rowRect.top(), rowRect.width(), separatorHeight());
}

QString richTextStyleSheet()
{
    using Core::Theme::Manager;
    using Core::Theme::Token;
    const QColor link = Manager::instance().color(Token::LinkText);
    const QColor mentionText = Manager::instance().color(Token::MentionText);
    const QColor mentionBg = Manager::instance().color(Token::MentionBg);
    const QString mentionBgRgba = QStringLiteral("rgba(%1, %2, %3, %4)")
                                          .arg(mentionBg.red())
                                          .arg(mentionBg.green())
                                          .arg(mentionBg.blue())
                                          .arg(QString::number(mentionBg.alphaF(), 'f', 3));
    const QFont codeFont = Manager::instance().font(Core::Theme::FontRole::Code);
    QString code = QStringLiteral("code { font-family: '%1';").arg(codeFont.family());
    if (codeFont.pointSizeF() > 0)
        code += QStringLiteral(" font-size: %1pt;").arg(codeFont.pointSizeF());
    code += QStringLiteral(" }");

    return QStringLiteral("a { color: %1; } "
                          ".mention { color: %2; background-color: %3; text-decoration: none; } ")
                   .arg(link.name(QColor::HexRgb))
                   .arg(mentionText.name(QColor::HexRgb))
                   .arg(mentionBgRgba) +
           code;
}

void setupDocument(QTextDocument &doc, const QString &htmlContent, const QFont &font, int textWidth)
{
    QString wrapped = QString("<div style=\"white-space: pre-wrap;\">%1</div>")
                              .arg(htmlContent);

    doc.setDefaultFont(font);
    doc.setDefaultStyleSheet(richTextStyleSheet());

    if (htmlContent.contains(QLatin1String("acheron-icon:view-thread"))) {
        const QColor threadColor = Core::Theme::Manager::instance().color(Core::Theme::Token::LinkText);
        doc.addResource(QTextDocument::ImageResource,
                        QUrl(QStringLiteral("acheron-icon:view-thread")),
                        Core::Theme::Icons::pixmap(Core::Theme::Icons::Name::Spool, 14, threadColor, 2.0));
    }

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
        int avatarTop = ctx.rowTop + blockTopPadding() + separatorOffset;
        layout.avatarRect = QRect(padding(), avatarTop, avatarSize(), avatarSize());
        layout.headerRect = QRect(textLeft, avatarTop - capDrop, textWidth, fm.height());
    }

    if (layout.showHeader) {
        layout.hitRegions.append({ HitRegion::Kind::Avatar, layout.avatarRect, -1, -1, {} });
        layout.hitRegions.append({ HitRegion::Kind::UsernameHeader, layout.headerRect, -1, -1, {} });
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

                    if (ctx.attachments[i].isVideo) {
                        int btnSize = videoDownloadBtnSize();
                        attLayout.downloadRect = QRect(attLayout.rect.right() - btnSize - 4,
                                                       attLayout.rect.top() + 4,
                                                       btnSize, btnSize);
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

    for (int ei = 0; ei < layout.embedLayouts.size(); ++ei) {
        const auto &el = layout.embedLayouts[ei];
        if (ei >= ctx.embeds.size())
            continue;
        const auto &embed = ctx.embeds[ei];

        if (el.hasThumbnail && !el.thumbnailRect.isNull())
            layout.hitRegions.append({ HitRegion::Kind::EmbedThumbnail, el.thumbnailRect, ei, -1, embed.thumbnailUrl.toString() });

        if (!embed.authorName.isEmpty() && !el.authorRect.isNull() && !embed.authorUrl.isEmpty())
            layout.hitRegions.append({ HitRegion::Kind::EmbedAuthor, el.authorRect, ei, -1, embed.authorUrl });

        if (!embed.title.isEmpty() && !el.titleRect.isNull())
            layout.hitRegions.append({ HitRegion::Kind::EmbedTitle, el.titleRect, ei, -1, embed.url });

        for (const auto &imgLayout : el.imageLayouts) {
            if (imgLayout.imageIndex >= embed.images.size())
                continue;
            layout.hitRegions.append({ HitRegion::Kind::EmbedImage, imgLayout.rect, ei, imgLayout.imageIndex, embed.images[imgLayout.imageIndex].url.toString() });
        }

        if (embed.images.isEmpty() && !embed.videoThumbnail.isNull() && embed.thumbnail.isNull() && !el.imagesRect.isNull())
            layout.hitRegions.append({ HitRegion::Kind::EmbedVideoThumbnail, el.imagesRect, ei, -1, embed.url });

        if (!embed.description.isEmpty() && !el.descriptionRect.isNull())
            layout.hitRegions.append({ HitRegion::Kind::EmbedDescription, el.descriptionRect, ei, -1, {} });

        for (const auto &fl : el.fieldLayouts) {
            if (fl.fieldIndex >= embed.fields.size())
                continue;
            const auto &field = embed.fields[fl.fieldIndex];
            if (!field.nameParsed.isEmpty() && !fl.nameRect.isNull())
                layout.hitRegions.append({ HitRegion::Kind::EmbedFieldName, fl.nameRect, ei, fl.fieldIndex, {} });
            if (!field.valueParsed.isEmpty() && !fl.valueRect.isNull())
                layout.hitRegions.append({ HitRegion::Kind::EmbedFieldValue, fl.valueRect, ei, fl.fieldIndex, {} });
        }
    }

    for (const auto &al : layout.imageLayouts) {
        const auto &att = ctx.attachments[al.index];

        if (att.isVideo && !al.downloadRect.isNull())
            layout.hitRegions.append({ HitRegion::Kind::AttachmentVideoDownload, al.downloadRect, al.index, -1, {} });

        auto kind = att.isVideo ? HitRegion::Kind::AttachmentVideo : HitRegion::Kind::AttachmentImage;
        layout.hitRegions.append({ kind, al.rect, al.index, -1, {} });
    }
    for (const auto &al : layout.fileLayouts)
        layout.hitRegions.append({ HitRegion::Kind::AttachmentFile, al.rect, al.index, -1, {} });

    for (const auto &rl : layout.reactionLayouts)
        layout.hitRegions.append({ HitRegion::Kind::Reaction, rl.pillRect, rl.reactionIndex, -1, {} });

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

static QString resolveEmbedAnchor(const ChatModel *model, const DocCacheKey &key,
                                  const QString &parsedHtml, const QFont &font,
                                  const QRect &rect, const QPoint &mousePos)
{
    if (parsedHtml.isEmpty() || !model)
        return {};
    QTextDocument *doc = model->getCachedDocument(key);
    QTextDocument localDoc;
    if (!doc) {
        localDoc.setDefaultFont(font);
        localDoc.setTextWidth(rect.width());
        localDoc.setHtml(parsedHtml);
        doc = &localDoc;
    }
    QPointF localPos = mousePos - rect.topLeft();
    return doc->documentLayout()->anchorAt(localPos);
}

std::optional<HitRegion> hitTest(const ResolvedLayout &resolved, const QPoint &mousePos)
{
    const auto &layout = resolved.layout;
    const auto &ctx = resolved.ctx;

    for (const auto &region : layout.hitRegions) {
        if (!region.rect.contains(mousePos))
            continue;

        switch (region.kind) {
        case HitRegion::Kind::EmbedTitle: {
            if (region.index >= 0 && region.index < ctx.embeds.size()) {
                const auto &embed = ctx.embeds[region.index];
                QFont titleFont = ctx.font;
                titleFont.setBold(true);
                QString link = resolveEmbedAnchor(ctx.model,
                                                  embedTitleDocKey(ctx.messageId, region.index),
                                                  embed.titleParsed, titleFont, region.rect, mousePos);
                if (!link.isEmpty())
                    return HitRegion{ HitRegion::Kind::EmbedLink, region.rect, region.index, -1, link };
            }

            if (region.url.isEmpty())
                continue;
            return region;
        }
        case HitRegion::Kind::EmbedDescription: {
            if (region.index >= 0 && region.index < ctx.embeds.size()) {
                const auto &embed = ctx.embeds[region.index];
                QString link = resolveEmbedAnchor(ctx.model,
                                                  embedDescDocKey(ctx.messageId, region.index),
                                                  embed.descriptionParsed, ctx.font, region.rect, mousePos);
                if (!link.isEmpty())
                    return HitRegion{ HitRegion::Kind::EmbedLink, region.rect, region.index, -1, link };
            }

            continue;
        }
        case HitRegion::Kind::EmbedFieldName: {
            if (region.index >= 0 && region.index < ctx.embeds.size() &&
                region.subIndex >= 0 && region.subIndex < ctx.embeds[region.index].fields.size()) {
                const auto &field = ctx.embeds[region.index].fields[region.subIndex];
                QFont nameFont = ctx.font;
                nameFont.setBold(true);
                QString link = resolveEmbedAnchor(ctx.model,
                                                  embedFieldNameDocKey(ctx.messageId, region.index, region.subIndex),
                                                  field.nameParsed, nameFont, region.rect, mousePos);
                if (!link.isEmpty())
                    return HitRegion{ HitRegion::Kind::EmbedLink, region.rect, region.index, region.subIndex, link };
            }
            continue;
        }
        case HitRegion::Kind::EmbedFieldValue: {
            if (region.index >= 0 && region.index < ctx.embeds.size() &&
                region.subIndex >= 0 && region.subIndex < ctx.embeds[region.index].fields.size()) {
                const auto &field = ctx.embeds[region.index].fields[region.subIndex];
                QString link = resolveEmbedAnchor(ctx.model,
                                                  embedFieldValueDocKey(ctx.messageId, region.index, region.subIndex),
                                                  field.valueParsed, ctx.font, region.rect, mousePos);
                if (!link.isEmpty())
                    return HitRegion{ HitRegion::Kind::EmbedLink, region.rect, region.index, region.subIndex, link };
            }
            continue;
        }
        default:
            return region;
        }
    }

    if (layout.textRect.contains(mousePos) && !ctx.htmlContent.isEmpty() && ctx.model) {
        QString link = getLinkAt(resolved, mousePos);
        if (!link.isEmpty())
            return HitRegion{ HitRegion::Kind::TextLink, layout.textRect, -1, -1, link };

        if (hitTestCharIndex(resolved, mousePos) >= 0)
            return HitRegion{ HitRegion::Kind::TextCursor, layout.textRect, -1, -1, {} };
    }

    return std::nullopt;
}

ResolvedLayout resolveLayout(const QAbstractItemView *view, const QModelIndex &index)
{
    ResolvedLayout result;
    if (!view || !index.isValid())
        return result;

    QRect rowRect = view->visualRect(index);

    LayoutContext &ctx = result.ctx;
    ctx.font = view->font();
    ctx.rowWidth = rowRect.width();
    ctx.rowTop = rowRect.top();
    ctx.showHeader = index.data(ChatModel::ShowHeaderRole).toBool();
    ctx.hasSeparator = index.data(ChatModel::DateSeparatorRole).toBool();
    ctx.htmlContent = index.data(ChatModel::HtmlRole).toString();
    ctx.replyData = index.data(ChatModel::ReplyDataRole).value<ReplyData>();
    ctx.attachments = index.data(ChatModel::AttachmentsRole).value<QList<AttachmentData>>();
    ctx.embeds = index.data(ChatModel::EmbedsRole).value<QList<EmbedData>>();
    ctx.reactions = index.data(ChatModel::ReactionsRole).value<QList<ReactionData>>();
    ctx.isSystemMessage = index.data(ChatModel::IsSystemMessageRole).toBool();
    ctx.model = qobject_cast<const ChatModel *>(index.model());
    ctx.messageId = index.data(ChatModel::MessageIdRole).toULongLong();

    result.layout = calculateMessageLayout(ctx);
    return result;
}

int hitTestCharIndex(const ResolvedLayout &resolved, const QPoint &viewportPos)
{
    const auto &layout = resolved.layout;
    const auto &ctx = resolved.ctx;

    if (ctx.htmlContent.isEmpty() || !ctx.model)
        return -1;

    QTextDocument *doc = ctx.model->getCachedDocument(bodyDocKey(ctx.messageId));
    QTextDocument localDoc;
    if (!doc) {
        setupDocument(localDoc, ctx.htmlContent, ctx.font, layout.textRect.width());
        doc = &localDoc;
    } else if (int(doc->textWidth()) != layout.textRect.width()) {
        doc->setTextWidth(layout.textRect.width());
    }

    QPointF local = viewportPos - layout.textRect.topLeft();

    if (local.y() < 0 || local.y() > doc->size().height())
        return -1;

    return doc->documentLayout()->hitTest(local, Qt::ExactHit);
}

int hitTestCharIndex(const QAbstractItemView *view, const QModelIndex &index, const QPoint &viewportPos)
{
    if (!view || !index.isValid())
        return -1;
    return hitTestCharIndex(resolveLayout(view, index), viewportPos);
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

QString getLinkAt(const ResolvedLayout &resolved, const QPoint &mousePos)
{
    const auto &layout = resolved.layout;
    const auto &ctx = resolved.ctx;

    if (ctx.htmlContent.isEmpty() || !ctx.model)
        return {};

    if (!layout.textRect.contains(mousePos))
        return {};

    QTextDocument *doc = ctx.model->getCachedDocument(bodyDocKey(ctx.messageId));
    QTextDocument localDoc;
    if (!doc) {
        setupDocument(localDoc, ctx.htmlContent, ctx.font, layout.textRect.width());
        doc = &localDoc;
    } else if (int(doc->textWidth()) != layout.textRect.width()) {
        doc->setTextWidth(layout.textRect.width());
    }

    QPointF localPos = mousePos - layout.textRect.topLeft();
    return doc->documentLayout()->anchorAt(localPos);
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
