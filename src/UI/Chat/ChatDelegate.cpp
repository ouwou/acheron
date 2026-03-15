#include "ChatDelegate.hpp"

#include "ChatModel.hpp"
#include "ChatLayout.hpp"
#include "ChatView.hpp"
#include "Core/ImageManager.hpp"

#include <algorithm>

namespace Acheron {
namespace UI {

static const QRegularExpression &emojiImgRegex()
{
    static const QRegularExpression re(
            R"lol(<img src="(https://cdn\.discordapp\.com/emojis/\d+\.webp\?size=\d+)"[^>]*width="(\d+)")lol");
    return re;
}

static const QString emojiCdnPrefix = QStringLiteral("https://cdn.discordapp.com/emojis/");

static void registerEmojiResources(QTextDocument &doc, const QString &html,
                                   Core::ImageManager *imageManager)
{
    if (!imageManager || !html.contains(emojiCdnPrefix))
        return;

    auto it = emojiImgRegex().globalMatch(html);
    while (it.hasNext()) {
        auto match = it.next();
        QUrl url(match.captured(1));
        int size = match.captured(2).toInt();
        QPixmap px = imageManager->get(url, QSize(size, size));
        doc.addResource(QTextDocument::ImageResource, url, px);
    }
}

static ChatLayout::LayoutContext buildLayoutContext(const QStyleOptionViewItem &option,
                                                    const QModelIndex &index)
{
    ChatLayout::LayoutContext ctx;
    ctx.font = option.font;
    ctx.rowWidth = option.rect.width();
    ctx.rowTop = option.rect.top();
    ctx.showHeader = index.data(ChatModel::ShowHeaderRole).toBool();
    ctx.hasSeparator = index.data(ChatModel::DateSeparatorRole).toBool();
    ctx.htmlContent = index.data(ChatModel::HtmlRole).toString();
    ctx.attachments = index.data(ChatModel::AttachmentsRole).value<QList<AttachmentData>>();
    ctx.embeds = index.data(ChatModel::EmbedsRole).value<QList<EmbedData>>();
    ctx.reactions = index.data(ChatModel::ReactionsRole).value<QList<ReactionData>>();
    ctx.replyData = index.data(ChatModel::ReplyDataRole).value<ReplyData>();
    ctx.isSystemMessage = index.data(ChatModel::IsSystemMessageRole).toBool();
    ctx.model = qobject_cast<const ChatModel *>(index.model());
    ctx.messageId = index.data(ChatModel::MessageIdRole).toULongLong();

    QDateTime editedTime = index.data(ChatModel::EditedTimestampRole).toDateTime();
    if (editedTime.isValid()) {
        QColor editedColor = option.palette.text().color().darker(200);
        ctx.htmlContent += QString(R"(<span style="color: %1"> (edited)</span>)").arg(editedColor.name());
    }

    return ctx;
}

void ChatDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
{
    painter->save();

    if (auto *chatModel = qobject_cast<const ChatModel *>(index.model()))
        chatModel->suppressImageFetch = false;

    ChatLayout::LayoutContext ctx = buildLayoutContext(option, index);
    ChatLayout::MessageLayout layout = ChatLayout::calculateMessageLayout(ctx);

    const QString username = index.data(ChatModel::UsernameRole).toString();
    const QPixmap avatar = qvariant_cast<QPixmap>(index.data(ChatModel::AvatarRole));
    const QDateTime timestamp = index.data(ChatModel::TimestampRole).toDateTime();

// debug paint
#if 0
    painter->setPen(Qt::red);
    painter->drawRect(layout.rowRect);
    if (layout.hasSeparator) {
        painter->setPen(Qt::yellow);
        painter->drawRect(layout.separatorRect);
    }
    if (layout.showHeader) {
        painter->setPen(Qt::green);
        painter->drawRect(layout.avatarRect);
        painter->setPen(Qt::green);
        painter->drawRect(layout.headerRect);
    }
    painter->setPen(Qt::white);
    painter->drawRect(layout.textRect);
#endif

    if (layout.hasSeparator) {
        painter->setPen(QPen(option.palette.alternateBase().color(), 1));
        int midY = layout.separatorRect.center().y();
        painter->drawLine(layout.separatorRect.left() + 10, midY, layout.separatorRect.right() - 10,
                          midY);

        QString dateText = timestamp.toString("MMMM d, yyyy");

        painter->setFont(option.font);
        QFontMetrics separatorFm(option.font);
        int textWidth = separatorFm.horizontalAdvance(dateText) + 20;
        QRect textBgRect(layout.separatorRect.center().x() - textWidth / 2,
                         layout.separatorRect.top(), textWidth, layout.separatorRect.height());

        painter->fillRect(textBgRect, option.palette.base());
        painter->setPen(option.palette.text().color());
        painter->drawText(layout.separatorRect, Qt::AlignCenter, dateText);
    }

    if (layout.hasReply && !layout.replyRect.isNull()) {
        ReplyData replyData = ctx.replyData;

        // Compute reply text font/metrics first so the connector aligns with the text
        QFont replyFont = option.font;
        replyFont.setPointSizeF(replyFont.pointSizeF() * 0.85);
        QFontMetrics replyFm(replyFont);

        int textX = layout.replyRect.left() + 4;
        int textY = layout.replyRect.top();
        int availWidth = layout.replyRect.width() - 4;

        // The vertical center of the reply text line
        int textMidY = textY + replyFm.height() / 2;

        // Draw the reply connector line (L-shaped)
        QColor lineColor = option.palette.text().color();
        lineColor.setAlpha(80);
        QPen replyPen(lineColor, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter->setPen(replyPen);

        int lineX = layout.avatarRect.center().x();
        int lineBottom = layout.avatarRect.top();
        int cornerRadius = 5;

        QPainterPath replyPath;
        replyPath.moveTo(lineX, lineBottom);
        replyPath.lineTo(lineX, textMidY + cornerRadius);
        replyPath.quadTo(lineX, textMidY, lineX + cornerRadius, textMidY);
        replyPath.lineTo(layout.replyRect.left(), textMidY);
        painter->drawPath(replyPath);

        // Draw reply text
        painter->setFont(replyFont);

        QColor replyTextColor = option.palette.text().color();
        replyTextColor.setAlpha(180);

        if (replyData.state == ReplyData::State::Present) {
            // Author name in bold, with role color if available
            QFont authorFont = replyFont;
            authorFont.setBold(true);
            painter->setFont(authorFont);
            QFontMetrics authorFm(authorFont);
            QColor authorColor = replyData.authorColor.isValid() ? replyData.authorColor : replyTextColor;
            painter->setPen(authorColor);
            QString authorName = replyData.authorName;
            int authorWidth = authorFm.horizontalAdvance(authorName);
            painter->drawText(textX, textY + authorFm.ascent(), authorName);

            // Content snippet
            painter->setFont(replyFont);
            painter->setPen(replyTextColor);
            int snippetX = textX + authorWidth + 6;
            int snippetWidth = availWidth - authorWidth - 6;
            if (snippetWidth > 0) {
                QString snippet = replyData.contentSnippet;
                snippet.replace('\n', ' ');
                QString elidedSnippet = replyFm.elidedText(snippet, Qt::ElideRight, snippetWidth);
                painter->drawText(snippetX, textY + replyFm.ascent(), elidedSnippet);
            }
        } else if (replyData.state == ReplyData::State::Deleted) {
            painter->setPen(replyTextColor);
            painter->drawText(textX, textY + replyFm.ascent(),
                              tr("Original message was deleted"));
        } else {
            painter->setPen(replyTextColor);
            painter->drawText(textX, textY + replyFm.ascent(),
                              tr("Unknown message"));
        }
    }

    if (layout.showHeader) {
        if (!avatar.isNull())
            painter->drawPixmap(layout.avatarRect, avatar);

        QFont headerFont = option.font;
        headerFont.setBold(true);
        painter->setFont(headerFont);
        QFontMetrics headerFm(headerFont);

        QColor headerColor;
        if (option.state & QStyle::State_Selected) {
            headerColor = option.palette.highlightedText().color();
        } else {
            QColor roleColor = index.data(ChatModel::UsernameColorRole).value<QColor>();
            headerColor = roleColor.isValid() ? roleColor : option.palette.text().color();
        }

        painter->setPen(headerColor);
        painter->drawText(layout.headerRect, Qt::AlignLeft | Qt::AlignTop, username);

        QFont timestampFont = option.font;
        timestampFont.setWeight(QFont::Light);
        painter->setFont(timestampFont);

        int usernameWidth = headerFm.horizontalAdvance(username);
        QRect timestampRect = layout.headerRect.adjusted(usernameWidth, 0, 0, 0);
        painter->setPen(option.palette.text().color().darker(150));
        painter->drawText(timestampRect, Qt::AlignLeft | Qt::AlignTop,
                          "  " + timestamp.toString("hh:mm"));
    }

    const auto *chatModel = qobject_cast<const ChatModel *>(index.model());
    Snowflake msgId = index.data(ChatModel::MessageIdRole).toULongLong();

    QFont bodyFont = option.font;
    if (ctx.isSystemMessage)
        bodyFont.setItalic(true);

    DocCacheKey bodyKey = bodyDocKey(msgId);
    QTextDocument *doc = chatModel->getCachedDocument(bodyKey);
    if (!doc) {
        doc = new QTextDocument;
        ChatLayout::setupDocument(*doc, ctx.htmlContent, bodyFont, layout.textRect.width());
        registerEmojiResources(*doc, ctx.htmlContent, imageManager);
        chatModel->cacheDocument(bodyKey, doc);
    } else if (int(doc->textWidth()) != layout.textRect.width()) {
        doc->setTextWidth(layout.textRect.width());
    }

    painter->translate(layout.textRect.topLeft());

    QAbstractTextDocumentLayout::PaintContext paintCtx;

    bool isPending = index.data(ChatModel::IsPendingRole).toBool();
    bool isErrored = index.data(ChatModel::IsErroredRole).toBool();

    QColor textColor;
    if (isErrored) {
        textColor = QColor(220, 50, 50);
    } else if (isPending) {
        textColor = option.palette.text().color().lighter(50);
    } else if (ctx.isSystemMessage) {
        textColor = option.palette.text().color();
        textColor.setAlpha(140);
    } else {
        textColor = (option.state & QStyle::State_Selected)
                            ? option.palette.highlightedText().color()
                            : option.palette.text().color();
    }
    paintCtx.palette.setColor(QPalette::Text, textColor);

    const ChatView *view = qobject_cast<const ChatView *>(option.widget);
    if (view && view->hasTextSelection()) {
        auto start = view->selectionStart();
        auto end = view->selectionEnd();
        int r = index.row();

        if (r >= start.row && r <= end.row) {
            int startChar = 0;
            int endChar = -1;

            if (r == start.row)
                startChar = start.index;
            if (r == end.row)
                endChar = end.index;

            QTextCursor cursor(doc);
            cursor.setPosition(startChar);

            if (endChar == -1)
                cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
            else
                cursor.setPosition(endChar, QTextCursor::KeepAnchor);

            QAbstractTextDocumentLayout::Selection sel;
            sel.cursor = cursor;
            sel.format.setBackground(option.palette.highlight());
            sel.format.setForeground(option.palette.highlightedText());
            paintCtx.selections.append(sel);
        }
    }

    doc->documentLayout()->draw(painter, paintCtx);

    painter->restore();
    painter->save();

    QList<AttachmentData> attachments = ctx.attachments;

    for (const auto &imgLayout : layout.imageLayouts) {
        if (imgLayout.index >= attachments.size())
            continue;

        const auto &att = attachments[imgLayout.index];
        bool isSingleImage =
                (layout.imageLayouts.size() == 1 &&
                 std::count_if(attachments.begin(), attachments.end(),
                               [](const AttachmentData &a) { return a.isImage; }) == 1);

        bool showBlurred = att.isSpoiler;
        if (showBlurred && chatModel->isSpoilerRevealed(att.id))
            showBlurred = false;

        if (!att.pixmap.isNull()) {
            QPixmap displayPixmap = att.pixmap;

            if (showBlurred)
                displayPixmap = ChatLayout::createBlurredPixmap(att.pixmap, 60);

            if (isSingleImage)
                painter->drawPixmap(imgLayout.rect, displayPixmap);
            else
                ChatLayout::drawCroppedPixmap(painter, imgLayout.rect, displayPixmap);

            if (showBlurred) {
                painter->fillRect(imgLayout.rect, QColor(0, 0, 0, 100));

                QFont spoilerFont = option.font;
                spoilerFont.setBold(true);
                spoilerFont.setPointSize(spoilerFont.pointSize() + 2);
                painter->setFont(spoilerFont);
                painter->setPen(Qt::white);
                painter->drawText(imgLayout.rect, Qt::AlignCenter, tr("SPOILER"));
            }
        } else {
            painter->fillRect(imgLayout.rect, QColor(60, 60, 60));
            painter->setPen(option.palette.text().color());
            painter->drawText(imgLayout.rect, Qt::AlignCenter, tr("Loading..."));
        }
    }

    constexpr int fileAttachmentPadding = 8;

    for (const auto &fileLayout : layout.fileLayouts) {
        if (fileLayout.index >= attachments.size())
            continue;

        const auto &att = attachments[fileLayout.index];
        QRect fileRect = fileLayout.rect;

        QColor bgColor = option.palette.alternateBase().color();
        painter->fillRect(fileRect, bgColor);

        painter->setPen(QPen(option.palette.mid().color(), 1));
        painter->drawRect(fileRect);

        QRect iconRect(fileRect.left() + fileAttachmentPadding,
                       fileRect.top() + fileAttachmentPadding, 32, 32);
        painter->fillRect(iconRect, option.palette.mid());
        painter->setPen(option.palette.text().color());
        painter->drawText(iconRect, Qt::AlignCenter, "📄");

        int textLeft = iconRect.right() + fileAttachmentPadding;
        QRect textAreaRect(textLeft, fileRect.top() + fileAttachmentPadding,
                           fileRect.width() - (textLeft - fileRect.left()) - fileAttachmentPadding,
                           fileRect.height() - fileAttachmentPadding * 2);

        QFont filenameFont = option.font;
        painter->setFont(filenameFont);
        painter->setPen(option.palette.text().color());
        QFontMetrics filenameFm(filenameFont);
        QString elidedFilename =
                filenameFm.elidedText(att.filename, Qt::ElideMiddle, textAreaRect.width());
        painter->drawText(textAreaRect.left(), textAreaRect.top() + filenameFm.ascent(),
                          elidedFilename);

        QFont sizeFont = option.font;
        sizeFont.setPointSize(sizeFont.pointSize() - 1);
        painter->setFont(sizeFont);
        painter->setPen(option.palette.placeholderText().color());
        QString sizeText = ChatLayout::formatFileSize(att.fileSizeBytes);
        QFontMetrics sizeFm(sizeFont);
        painter->drawText(textAreaRect.left(),
                          textAreaRect.top() + filenameFm.height() + sizeFm.ascent(), sizeText);
    }

    QList<EmbedData> embeds = ctx.embeds;

    for (int embedIdx = 0; embedIdx < layout.embedLayouts.size() && embedIdx < embeds.size();
         ++embedIdx) {
        const auto &embedLayout = layout.embedLayouts[embedIdx];
        const auto &embed = embeds[embedIdx];

        if (embed.type == EmbedType::Gifv) {
            if (!embedLayout.imagesRect.isNull() && !embed.thumbnail.isNull()) {
                QPixmap scaledThumb = embed.thumbnail.scaled(
                        embed.thumbnailSize * embed.thumbnail.devicePixelRatio(),
                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter->drawPixmap(embedLayout.imagesRect.topLeft(), scaledThumb);
            }

            QFont gifFont = option.font;
            gifFont.setPointSize(gifFont.pointSize() - 2);
            painter->setFont(gifFont);
            painter->setPen(option.palette.placeholderText().color());
            QFontMetrics gifFm(gifFont);
            int gifLabelTop = embedLayout.imagesRect.isNull() ? embedLayout.embedRect.top()
                                                              : embedLayout.imagesRect.bottom();
            painter->drawText(embedLayout.embedRect.left(), gifLabelTop + gifFm.ascent() + 2,
                              "GIF");
            continue;
        }

        if (embed.type == EmbedType::Image) {
            if (!embedLayout.imagesRect.isNull() && !embed.thumbnail.isNull()) {
                QPixmap scaledThumb = embed.thumbnail.scaled(
                        embed.thumbnailSize * embed.thumbnail.devicePixelRatio(),
                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter->drawPixmap(embedLayout.imagesRect.topLeft(), scaledThumb);
            }
            continue;
        }

        painter->fillRect(embedLayout.embedRect, option.palette.base().color().darker(110));

        QRect borderRect(embedLayout.embedRect.left(), embedLayout.embedRect.top(),
                         ChatLayout::embedBorderWidth(), embedLayout.embedRect.height());
        painter->fillRect(borderRect, embed.color);

        if (embedLayout.hasThumbnail && !embedLayout.thumbnailRect.isNull()) {
            QPixmap thumb = !embed.thumbnail.isNull() ? embed.thumbnail : embed.videoThumbnail;
            if (!thumb.isNull()) {
                QPixmap scaledThumb = thumb.scaled(embedLayout.thumbnailRect.size(),
                                                   Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter->drawPixmap(embedLayout.thumbnailRect.topLeft(), scaledThumb);
            }
        }

        if (!embed.providerName.isEmpty() && !embedLayout.providerRect.isNull()) {
            QFont providerFont = option.font;
            providerFont.setPointSize(providerFont.pointSize() - 2);
            painter->setFont(providerFont);
            painter->setPen(option.palette.placeholderText().color());
            QFontMetrics providerFm(providerFont);
            painter->drawText(embedLayout.providerRect.left(),
                              embedLayout.providerRect.top() + providerFm.ascent(),
                              embed.providerName);
        }

        if (!embed.authorName.isEmpty() && !embedLayout.authorRect.isNull()) {
            int authorX = embedLayout.authorRect.left();
            int authorY = embedLayout.authorRect.top();
            if (!embed.authorIcon.isNull()) {
                QRect iconRect(authorX, authorY, ChatLayout::authorIconSize(),
                               ChatLayout::authorIconSize());
                painter->drawPixmap(iconRect, embed.authorIcon);
                authorX += ChatLayout::authorIconSize() + 6;
            }
            QFont authorFont = option.font;
            authorFont.setPointSize(authorFont.pointSize() - 1);
            authorFont.setBold(true);
            painter->setFont(authorFont);
            painter->setPen(option.palette.text().color());
            QFontMetrics authorFm(authorFont);
            int textY = authorY + (ChatLayout::authorIconSize() - authorFm.height()) / 2 +
                        authorFm.ascent();
            painter->drawText(authorX, textY, embed.authorName);
        }

        if (!embed.title.isEmpty() && !embedLayout.titleRect.isNull()) {
            QFont titleFont = option.font;
            titleFont.setBold(true);
            painter->setFont(titleFont);
            QColor titleColor = !embed.url.isEmpty() ? option.palette.link().color()
                                                     : option.palette.text().color();
            painter->setPen(titleColor);

            QString titleHtml = !embed.titleParsed.isEmpty() ? embed.titleParsed : embed.title;
            DocCacheKey titleKey = embedTitleDocKey(msgId, embedIdx);
            QTextDocument *titleDoc = chatModel->getCachedDocument(titleKey);
            if (!titleDoc) {
                titleDoc = new QTextDocument;
                titleDoc->setDefaultFont(titleFont);
                titleDoc->setTextWidth(embedLayout.titleRect.width());
                registerEmojiResources(*titleDoc, titleHtml, imageManager);
                titleDoc->setHtml(titleHtml);
                chatModel->cacheDocument(titleKey, titleDoc);
            } else if (int(titleDoc->textWidth()) != embedLayout.titleRect.width()) {
                titleDoc->setTextWidth(embedLayout.titleRect.width());
            }

            painter->save();
            painter->translate(embedLayout.titleRect.topLeft());
            QAbstractTextDocumentLayout::PaintContext titleCtx;
            titleCtx.palette.setColor(QPalette::Text, titleColor);
            titleDoc->documentLayout()->draw(painter, titleCtx);
            painter->restore();
        }

        if (!embed.description.isEmpty() && !embedLayout.descriptionRect.isNull()) {
            QFont descFont = option.font;
            painter->setFont(descFont);
            painter->setPen(option.palette.text().color());

            QString descHtml = !embed.descriptionParsed.isEmpty() ? embed.descriptionParsed
                                                                  : embed.description;
            DocCacheKey descKey = embedDescDocKey(msgId, embedIdx);
            QTextDocument *descDoc = chatModel->getCachedDocument(descKey);
            if (!descDoc) {
                descDoc = new QTextDocument;
                descDoc->setDefaultFont(descFont);
                descDoc->setTextWidth(embedLayout.descriptionRect.width());
                registerEmojiResources(*descDoc, descHtml, imageManager);
                descDoc->setHtml(descHtml);
                chatModel->cacheDocument(descKey, descDoc);
            } else if (int(descDoc->textWidth()) != embedLayout.descriptionRect.width()) {
                descDoc->setTextWidth(embedLayout.descriptionRect.width());
            }

            painter->save();
            painter->translate(embedLayout.descriptionRect.topLeft());
            QAbstractTextDocumentLayout::PaintContext descCtx;
            descCtx.palette.setColor(QPalette::Text, option.palette.text().color());
            descDoc->documentLayout()->draw(painter, descCtx);
            painter->restore();
        }

        QFont fieldNameFont = option.font;
        fieldNameFont.setBold(true);
        QFontMetrics fieldNameFm(fieldNameFont);

        for (const auto &fieldLayout : embedLayout.fieldLayouts) {
            if (fieldLayout.fieldIndex >= embed.fields.size())
                continue;

            const auto &field = embed.fields[fieldLayout.fieldIndex];
            int fi = fieldLayout.fieldIndex;

            QString nameHtml = !field.nameParsed.isEmpty() ? field.nameParsed : field.name;
            DocCacheKey nameKey = embedFieldNameDocKey(msgId, embedIdx, fi);
            QTextDocument *nameDoc = chatModel->getCachedDocument(nameKey);
            if (!nameDoc) {
                nameDoc = new QTextDocument;
                nameDoc->setDefaultFont(fieldNameFont);
                nameDoc->setTextWidth(fieldLayout.nameRect.width());
                registerEmojiResources(*nameDoc, nameHtml, imageManager);
                nameDoc->setHtml(nameHtml);
                chatModel->cacheDocument(nameKey, nameDoc);
            } else if (int(nameDoc->textWidth()) != fieldLayout.nameRect.width()) {
                nameDoc->setTextWidth(fieldLayout.nameRect.width());
            }

            painter->save();
            painter->translate(fieldLayout.nameRect.topLeft());
            QAbstractTextDocumentLayout::PaintContext nameCtx;
            nameCtx.palette.setColor(QPalette::Text, option.palette.text().color());
            nameDoc->documentLayout()->draw(painter, nameCtx);
            painter->restore();

            QString valueHtml = !field.valueParsed.isEmpty() ? field.valueParsed : field.value;
            DocCacheKey valueKey = embedFieldValueDocKey(msgId, embedIdx, fi);
            QTextDocument *valueDoc = chatModel->getCachedDocument(valueKey);
            if (!valueDoc) {
                valueDoc = new QTextDocument;
                valueDoc->setDefaultFont(option.font);
                valueDoc->setTextWidth(fieldLayout.valueRect.width());
                registerEmojiResources(*valueDoc, valueHtml, imageManager);
                valueDoc->setHtml(valueHtml);
                chatModel->cacheDocument(valueKey, valueDoc);
            } else if (int(valueDoc->textWidth()) != fieldLayout.valueRect.width()) {
                valueDoc->setTextWidth(fieldLayout.valueRect.width());
            }

            painter->save();
            painter->translate(fieldLayout.valueRect.topLeft());
            QAbstractTextDocumentLayout::PaintContext valueCtx;
            valueCtx.palette.setColor(QPalette::Text, option.palette.text().color().darker(110));
            valueDoc->documentLayout()->draw(painter, valueCtx);
            painter->restore();
        }

        if (!embed.images.isEmpty()) {
            for (const auto &imgLayout : embedLayout.imageLayouts) {
                if (imgLayout.imageIndex >= embed.images.size())
                    continue;

                const auto &img = embed.images[imgLayout.imageIndex];
                bool isSingleImage = (embed.images.size() == 1);

                if (!img.pixmap.isNull()) {
                    if (isSingleImage) {
                        QPixmap scaledImage =
                                img.pixmap.scaled(img.displaySize * img.pixmap.devicePixelRatio(),
                                                  Qt::KeepAspectRatio, Qt::SmoothTransformation);
                        painter->drawPixmap(imgLayout.rect.topLeft(), scaledImage);
                    } else {
                        ChatLayout::drawCroppedPixmap(painter, imgLayout.rect, img.pixmap);
                    }
                } else {
                    painter->fillRect(imgLayout.rect, QColor(60, 60, 60));
                    painter->setPen(option.palette.text().color());
                    painter->drawText(imgLayout.rect, Qt::AlignCenter, "Loading...");
                }
            }
        } else if (!embed.videoThumbnail.isNull() && embed.thumbnail.isNull() &&
                   !embedLayout.imagesRect.isNull()) {
            QPixmap scaledVideo = embed.videoThumbnail.scaled(
                    embedLayout.imagesRect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            painter->drawPixmap(embedLayout.imagesRect.topLeft(), scaledVideo);

            int playSize = std::min(
                    48,
                    std::min(embedLayout.imagesRect.width(), embedLayout.imagesRect.height()) / 2);
            QRect playRect(embedLayout.imagesRect.center().x() - playSize / 2,
                           embedLayout.imagesRect.center().y() - playSize / 2, playSize, playSize);
            painter->setBrush(QColor(0, 0, 0, 180));
            painter->setPen(Qt::NoPen);
            painter->drawEllipse(playRect);
            painter->setPen(Qt::white);
            QPolygon triangle;
            int triOffset = playSize / 4;
            triangle << QPoint(playRect.left() + triOffset + 4, playRect.top() + triOffset)
                     << QPoint(playRect.left() + triOffset + 4, playRect.bottom() - triOffset)
                     << QPoint(playRect.right() - triOffset, playRect.center().y());
            painter->setBrush(Qt::white);
            painter->drawPolygon(triangle);
        }

        if (!embed.footerText.isEmpty() && !embedLayout.footerRect.isNull()) {
            int footerX = embedLayout.footerRect.left();
            int footerY = embedLayout.footerRect.top();
            if (!embed.footerIcon.isNull()) {
                QRect iconRect(footerX, footerY, ChatLayout::footerIconSize(),
                               ChatLayout::footerIconSize());
                painter->drawPixmap(iconRect, embed.footerIcon);
                footerX += ChatLayout::footerIconSize() + 6;
            }
            QFont footerFont = option.font;
            footerFont.setPointSize(footerFont.pointSize() - 2);
            painter->setFont(footerFont);
            painter->setPen(option.palette.placeholderText().color());
            QFontMetrics footerFm(footerFont);

            QString footerText = embed.footerText;
            if (embed.timestamp.isValid())
                footerText += " • " + embed.timestamp.toString("MMM d, yyyy h:mm AP");

            int textY = footerY + (ChatLayout::footerIconSize() - footerFm.height()) / 2 +
                        footerFm.ascent();
            painter->drawText(footerX, textY, footerText);
        }
    }

    QList<ReactionData> reactions = ctx.reactions;

    for (const auto &reactionLayout : layout.reactionLayouts) {
        if (reactionLayout.reactionIndex >= reactions.size())
            continue;

        const auto &reaction = reactions[reactionLayout.reactionIndex];

        QColor pillBg;
        if (reaction.isBurst && reaction.burstTintColor.isValid()) {
            pillBg = reaction.burstTintColor;
            pillBg.setAlpha(40);
        } else {
            pillBg = option.palette.alternateBase().color();
        }

        QColor borderColor;
        int borderWidth;
        if (reaction.me) {
            borderColor = option.palette.highlight().color();
            borderWidth = 1;
        } else {
            borderColor = option.palette.mid().color();
            borderWidth = 1;
        }

        painter->setPen(QPen(borderColor, borderWidth));
        painter->setBrush(pillBg);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->drawRoundedRect(reactionLayout.pillRect, 6, 6);
        painter->setRenderHint(QPainter::Antialiasing, false);

        if (reaction.emojiId.isValid()) {
            if (!reaction.emojiPixmap.isNull())
                painter->drawPixmap(reactionLayout.emojiRect, reaction.emojiPixmap);
        } else {
            // render smaller than the rect to fit within pill
            QFont emojiFont = option.font;
            emojiFont.setPixelSize(ChatLayout::reactionEmojiSize() - 4);
            painter->setFont(emojiFont);
            painter->setPen(option.palette.text().color());
            painter->drawText(reactionLayout.emojiRect, Qt::AlignCenter, reaction.emojiName);
        }

        QFont countFont = option.font;
        countFont.setPointSizeF(countFont.pointSizeF() * 0.85);
        painter->setFont(countFont);

        QColor countColor;
        if (reaction.me)
            countColor = option.palette.highlight().color();
        else
            countColor = option.palette.text().color();

        painter->setPen(countColor);
        painter->drawText(reactionLayout.countRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QString::number(reaction.count));
    }

    painter->restore();
}

QSize ChatDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    int viewportWidth = 400;
    const ChatView *chatView = nullptr;
    if (option.widget) {
        if (auto view = qobject_cast<const QAbstractItemView *>(option.widget)) {
            viewportWidth = view->viewport()->width();
            chatView = qobject_cast<const ChatView *>(view);
        } else {
            viewportWidth = option.widget->width();
        }
    }

    bool isEditing = chatView && chatView->editingRow() == index.row();

    QSize cached = index.data(ChatModel::CachedSizeRole).toSize();
    if (cached.isValid() && cached.width() == viewportWidth && !isEditing)
        return cached;

    const auto *chatModel = qobject_cast<const ChatModel *>(index.model());
    if (chatModel)
        chatModel->suppressImageFetch = true;

    ChatLayout::LayoutContext ctx = buildLayoutContext(option, index);

    if (chatModel)
        chatModel->suppressImageFetch = false;

    ctx.rowWidth = viewportWidth;
    ctx.rowTop = 0;

    ChatLayout::MessageLayout layout = ChatLayout::calculateMessageLayout(ctx);

    int height = layout.totalHeight;
    if (isEditing)
        height = qMax(height, height + ChatView::InlineEditMinHeight);

    QSize size(viewportWidth, height);

    if (!isEditing) {
        auto model = const_cast<QAbstractItemModel *>(index.model());
        const auto prevSize = index.data(ChatModel::CachedSizeRole).toSize();
        if (size != prevSize)
            model->setData(index, size, ChatModel::CachedSizeRole);
    }

    return size;
}

} // namespace UI
} // namespace Acheron
