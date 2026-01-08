#include "ChatDelegate.hpp"

#include "ChatModel.hpp"
#include "ChatLayout.hpp"
#include "ChatView.hpp"

namespace Acheron {
namespace UI {

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
    return ctx;
}

void ChatDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
{
    painter->save();

    const QString html = index.data(ChatModel::HtmlRole).toString();
    const QString username = index.data(ChatModel::UsernameRole).toString();
    const QPixmap avatar = qvariant_cast<QPixmap>(index.data(ChatModel::AvatarRole));
    const QDateTime timestamp = index.data(ChatModel::TimestampRole).toDateTime();
    const bool showHeader = index.data(ChatModel::ShowHeaderRole).toBool();
    const bool hasSeparator = index.data(ChatModel::DateSeparatorRole).toBool();

    QFontMetrics fm(option.font);
    const QRect rowRect = option.rect;

    const QRect avatarRect = ChatLayout::avatarRectForRow(rowRect, hasSeparator);
    const QRect headerRect = ChatLayout::headerRectForRow(rowRect, fm, hasSeparator);
    const QRect textRect = ChatLayout::textRectForRow(rowRect, showHeader, fm, hasSeparator);

    if (hasSeparator) {
        QRect separatorRect = ChatLayout::dateSeparatorRectForRow(rowRect);

        painter->setPen(QPen(option.palette.alternateBase().color(), 1));
        int midY = separatorRect.center().y();
        painter->drawLine(separatorRect.left() + 10, midY, separatorRect.right() - 10, midY);

        QString dateText = timestamp.toString("MMMM d, yyyy");

        painter->setFont(option.font);
        QFontMetrics separatorFm(option.font);
        int textWidth = separatorFm.horizontalAdvance(dateText) + 20;
        QRect textBgRect(separatorRect.center().x() - textWidth / 2, separatorRect.top(), textWidth,
                         separatorRect.height());

        painter->fillRect(textBgRect, option.palette.base());
        painter->setPen(option.palette.text().color());
        painter->drawText(separatorRect, Qt::AlignCenter, dateText);
    }

    if (showHeader) {
        if (!avatar.isNull())
            painter->drawPixmap(avatarRect, avatar);

        QFont headerFont = option.font;
        headerFont.setBold(true);
        painter->setFont(headerFont);

        QColor headerColor = (option.state & QStyle::State_Selected)
                                     ? option.palette.highlightedText().color()
                                     : option.palette.text().color();
        painter->setPen(headerColor);

        QString header = username + "  " + timestamp.toString("hh:mm");
        painter->drawText(headerRect, Qt::AlignLeft | Qt::AlignTop, header);
    }

    QTextDocument doc;
    ChatLayout::setupDocument(doc, html, option.font, textRect.width());

    painter->translate(textRect.topLeft());

    QAbstractTextDocumentLayout::PaintContext ctx;
    QColor textColor = (option.state & QStyle::State_Selected)
                               ? option.palette.highlightedText().color()
                               : option.palette.text().color();
    ctx.palette.setColor(QPalette::Text, textColor);

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

            QTextCursor cursor(&doc);
            cursor.setPosition(startChar);

            if (endChar == -1)
                cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
            else
                cursor.setPosition(endChar, QTextCursor::KeepAnchor);

            QAbstractTextDocumentLayout::Selection sel;
            sel.cursor = cursor;
            sel.format.setBackground(option.palette.highlight());
            sel.format.setForeground(option.palette.highlightedText());
            ctx.selections.append(sel);
        }
    }

    doc.documentLayout()->draw(painter, ctx);

    painter->restore();
    painter->save();

    int realTextHeight = int(std::ceil(doc.size().height()));
    int currentTop = textRect.top() + realTextHeight + ChatLayout::padding();

    QList<AttachmentData> attachments =
            index.data(ChatModel::AttachmentsRole).value<QList<AttachmentData>>();

    if (!attachments.isEmpty()) {
        QList<AttachmentData> images;
        QList<AttachmentData> files;
        for (const auto &att : attachments) {
            if (att.isImage)
                images.append(att);
            else
                files.append(att);
        }

        if (!images.isEmpty()) {
            ChatLayout::AttachmentGridLayout grid =
                    ChatLayout::calculateAttachmentGrid(images.size(), textRect.width());
            bool isSingleImage = (images.size() == 1);

            for (const auto &cell : grid.cells) {
                if (cell.attachmentIndex >= images.size())
                    continue;

                const auto &att = images[cell.attachmentIndex];
                QRect imgRect = cell.rect.translated(textRect.left(), currentTop);

                if (!att.pixmap.isNull()) {
                    if (isSingleImage) {
                        QRect actualRect(imgRect.x(), imgRect.y(), att.displaySize.width(),
                                         att.displaySize.height());
                        painter->drawPixmap(actualRect, att.pixmap);
                    } else {
                        ChatLayout::drawCroppedPixmap(painter, imgRect, att.pixmap);
                    }
                } else {
                    painter->fillRect(imgRect, QColor(60, 60, 60));
                    painter->setPen(option.palette.text().color());
                    painter->drawText(imgRect, Qt::AlignCenter, "Loading...");
                }
            }

            if (isSingleImage && !images.isEmpty())
                currentTop += images[0].displaySize.height() + ChatLayout::padding();
            else if (!images.isEmpty())
                currentTop += grid.totalHeight + ChatLayout::padding();
        }

        constexpr int fileAttachmentPadding = 8;
        int fileWidth = std::min(textRect.width(), ChatLayout::maxAttachmentWidth());

        for (const auto &att : files) {
            QRect fileRect(textRect.left(), currentTop, fileWidth,
                           ChatLayout::fileAttachmentHeight());

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
                               fileRect.width() - (textLeft - fileRect.left()) -
                                       fileAttachmentPadding,
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

            currentTop = fileRect.bottom() + ChatLayout::padding();
        }
    }

    QList<EmbedData> embeds = index.data(ChatModel::EmbedsRole).value<QList<EmbedData>>();
    if (!embeds.isEmpty()) {
        for (const auto &embed : embeds) {
            ChatLayout::EmbedLayout embedLayout = ChatLayout::calculateEmbedLayout(
                    embed, option.font, textRect.width(), currentTop);

            QRect embedRect = embedLayout.embedRect;
            embedRect.moveLeft(textRect.left());

            if (embed.type == EmbedType::Gifv) {
                int gifTop = embedRect.top();

                if (!embed.thumbnail.isNull()) {
                    QPixmap scaledThumb = embed.thumbnail.scaled(
                            embed.thumbnailSize * embed.thumbnail.devicePixelRatio(),
                            Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    painter->drawPixmap(textRect.left(), gifTop, scaledThumb);
                    gifTop += embed.thumbnailSize.height();
                }

                QFont gifFont = option.font;
                gifFont.setPointSize(gifFont.pointSize() - 2);
                painter->setFont(gifFont);
                painter->setPen(option.palette.placeholderText().color());
                QFontMetrics gifFm(gifFont);
                painter->drawText(textRect.left(), gifTop + gifFm.ascent() + 2, "GIF");

                currentTop += embedLayout.totalHeight + ChatLayout::padding();
                continue;
            }

            QColor bgColor = option.palette.base().color().darker(110);
            painter->fillRect(embedRect, bgColor);

            QRect borderRect(embedRect.left(), embedRect.top(), ChatLayout::embedBorderWidth(),
                             embedRect.height());
            painter->fillRect(borderRect, embed.color);

            int contentLeft =
                    embedRect.left() + ChatLayout::embedBorderWidth() + ChatLayout::embedPadding();
            int contentTop = embedRect.top() + ChatLayout::embedPadding();
            int contentWidth = embedLayout.contentWidth;
            int currentY = contentTop;

            if (embedLayout.hasThumbnail) {
                QPixmap thumb = !embed.thumbnail.isNull() ? embed.thumbnail : embed.videoThumbnail;
                QSize thumbDisplaySize =
                        !embed.thumbnail.isNull() ? embed.thumbnailSize : embed.videoThumbnailSize;
                if (!thumb.isNull()) {
                    int thumbX = contentLeft + contentWidth + ChatLayout::embedPadding();
                    QPixmap scaledThumb = thumb.scaled(thumbDisplaySize, Qt::KeepAspectRatio,
                                                       Qt::SmoothTransformation);
                    painter->drawPixmap(thumbX, contentTop, scaledThumb);
                }
            }

            if (!embed.providerName.isEmpty()) {
                QFont providerFont = option.font;
                providerFont.setPointSize(providerFont.pointSize() - 2);
                painter->setFont(providerFont);
                painter->setPen(option.palette.placeholderText().color());
                QFontMetrics providerFm(providerFont);
                painter->drawText(contentLeft, currentY + providerFm.ascent(), embed.providerName);
                currentY += embedLayout.providerHeight;
            }

            if (!embed.authorName.isEmpty()) {
                int authorX = contentLeft;
                if (!embed.authorIcon.isNull()) {
                    QRect iconRect(authorX, currentY, ChatLayout::authorIconSize(),
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
                int textY = currentY + (ChatLayout::authorIconSize() - authorFm.height()) / 2 +
                            authorFm.ascent();
                painter->drawText(authorX, textY, embed.authorName);
                currentY += embedLayout.authorHeight;
            }

            if (!embed.title.isEmpty()) {
                QFont titleFont = option.font;
                titleFont.setBold(true);
                painter->setFont(titleFont);
                if (!embed.url.isEmpty())
                    painter->setPen(QColor(0, 168, 252));
                else
                    painter->setPen(option.palette.text().color());
                QFontMetrics titleFm(titleFont);
                QString elidedTitle = titleFm.elidedText(embed.title, Qt::ElideRight, contentWidth);
                painter->drawText(contentLeft, currentY + titleFm.ascent(), elidedTitle);
                currentY += embedLayout.titleHeight;
            }

            if (!embed.description.isEmpty()) {
                QFont descFont = option.font;
                painter->setFont(descFont);
                painter->setPen(option.palette.text().color());

                QTextDocument descDoc;
                descDoc.setDefaultFont(descFont);
                descDoc.setTextWidth(contentWidth);
                descDoc.setPlainText(embed.description);

                painter->save();
                painter->translate(contentLeft, currentY);
                QAbstractTextDocumentLayout::PaintContext descCtx;
                descCtx.palette.setColor(QPalette::Text, option.palette.text().color());
                descDoc.documentLayout()->draw(painter, descCtx);
                painter->restore();

                currentY += embedLayout.descriptionHeight;
            }

            if (!embed.fields.isEmpty()) {
                QFont fieldNameFont = option.font;
                fieldNameFont.setBold(true);
                QFontMetrics fieldNameFm(fieldNameFont);
                int fieldWidth = (contentWidth - 2 * ChatLayout::fieldSpacing()) / 3;
                int fieldX = contentLeft;
                int fieldsInRow = 0;
                int rowStartY = currentY;
                int maxRowHeight = 0;

                for (const auto &field : embed.fields) {
                    if (!field.isInline) {
                        if (fieldsInRow > 0) {
                            currentY = rowStartY + maxRowHeight + ChatLayout::fieldSpacing();
                            fieldX = contentLeft;
                            fieldsInRow = 0;
                            maxRowHeight = 0;
                            rowStartY = currentY;
                        }

                        painter->setFont(fieldNameFont);
                        painter->setPen(option.palette.text().color());
                        painter->drawText(fieldX, currentY + fieldNameFm.ascent(), field.name);

                        QTextDocument valueDoc;
                        valueDoc.setDefaultFont(option.font);
                        valueDoc.setTextWidth(contentWidth);
                        valueDoc.setPlainText(field.value);

                        painter->save();
                        painter->translate(fieldX, currentY + fieldNameFm.height() + 2);
                        QAbstractTextDocumentLayout::PaintContext valueCtx;
                        valueCtx.palette.setColor(QPalette::Text,
                                                  option.palette.text().color().darker(110));
                        valueDoc.documentLayout()->draw(painter, valueCtx);
                        painter->restore();

                        int valueHeight = int(std::ceil(valueDoc.size().height()));
                        int fieldHeight = fieldNameFm.height() + 2 + valueHeight;
                        currentY += fieldHeight + ChatLayout::fieldSpacing();
                        rowStartY = currentY;
                    } else {
                        if (fieldsInRow >= 3) {
                            currentY = rowStartY + maxRowHeight + ChatLayout::fieldSpacing();
                            fieldX = contentLeft;
                            fieldsInRow = 0;
                            maxRowHeight = 0;
                            rowStartY = currentY;
                        }

                        painter->setFont(fieldNameFont);
                        painter->setPen(option.palette.text().color());
                        QString nameText =
                                fieldNameFm.elidedText(field.name, Qt::ElideRight, fieldWidth);
                        painter->drawText(fieldX, currentY + fieldNameFm.ascent(), nameText);

                        QTextDocument valueDoc;
                        valueDoc.setDefaultFont(option.font);
                        valueDoc.setTextWidth(fieldWidth);
                        valueDoc.setPlainText(field.value);

                        painter->save();
                        painter->translate(fieldX, currentY + fieldNameFm.height() + 2);
                        QAbstractTextDocumentLayout::PaintContext valueCtx;
                        valueCtx.palette.setColor(QPalette::Text,
                                                  option.palette.text().color().darker(110));
                        valueDoc.documentLayout()->draw(painter, valueCtx);
                        painter->restore();

                        int valueHeight = int(std::ceil(valueDoc.size().height()));
                        int fieldHeight = fieldNameFm.height() + 2 + valueHeight;
                        maxRowHeight = std::max(maxRowHeight, fieldHeight);
                        fieldX += fieldWidth + ChatLayout::fieldSpacing();
                        fieldsInRow++;
                    }
                }

                if (fieldsInRow > 0)
                    currentY = rowStartY + maxRowHeight + ChatLayout::fieldSpacing();
            }

            // multiple images can come from merged embeds
            if (!embed.images.isEmpty()) {
                bool isSingleImage = (embed.images.size() == 1);

                if (isSingleImage) {
                    const auto &img = embed.images[0];
                    if (!img.pixmap.isNull()) {
                        QPixmap scaledImage =
                                img.pixmap.scaled(img.displaySize * img.pixmap.devicePixelRatio(),
                                                  Qt::KeepAspectRatio, Qt::SmoothTransformation);
                        painter->drawPixmap(contentLeft, currentY, scaledImage);
                        currentY += scaledImage.height();
                    }
                } else {
                    ChatLayout::AttachmentGridLayout grid =
                            ChatLayout::calculateAttachmentGrid(embed.images.size(), contentWidth);

                    for (const auto &cell : grid.cells) {
                        if (cell.attachmentIndex >= embed.images.size())
                            continue;

                        const auto &img = embed.images[cell.attachmentIndex];
                        QRect imgRect = cell.rect.translated(contentLeft, currentY);

                        if (!img.pixmap.isNull()) {
                            ChatLayout::drawCroppedPixmap(painter, imgRect, img.pixmap);
                        } else {
                            painter->fillRect(imgRect, QColor(60, 60, 60));
                            painter->setPen(option.palette.text().color());
                            painter->drawText(imgRect, Qt::AlignCenter, "Loading...");
                        }
                    }
                    currentY += grid.totalHeight;
                }
            } else if (!embed.videoThumbnail.isNull() && embed.thumbnail.isNull()) {
                QPixmap scaledVideo = embed.videoThumbnail.scaled(
                        embed.videoThumbnailSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                QRect videoRect(contentLeft, currentY, scaledVideo.width(), scaledVideo.height());
                painter->drawPixmap(contentLeft, currentY, scaledVideo);

                // todo i vibed this but idk how it looks yet lol
                int playSize = std::min(48, std::min(videoRect.width(), videoRect.height()) / 2);
                QRect playRect(videoRect.center().x() - playSize / 2,
                               videoRect.center().y() - playSize / 2, playSize, playSize);
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

                currentY += embed.videoThumbnailSize.height();
            }

            if (!embed.footerText.isEmpty()) {
                int footerX = contentLeft;
                if (!embed.footerIcon.isNull()) {
                    QRect iconRect(footerX, currentY, ChatLayout::footerIconSize(),
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

                int textY = currentY + (ChatLayout::footerIconSize() - footerFm.height()) / 2 +
                            footerFm.ascent();
                painter->drawText(footerX, textY, footerText);
            }

            currentTop += embedLayout.totalHeight + ChatLayout::padding();
        }
    }

    painter->restore();
}

QSize ChatDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    int viewportWidth = 400;
    if (option.widget) {
        if (auto view = qobject_cast<const QAbstractItemView *>(option.widget))
            viewportWidth = view->viewport()->width();
        else
            viewportWidth = option.widget->width();
    }

    QSize cached = index.data(ChatModel::CachedSizeRole).toSize();
    if (cached.isValid() && cached.width() == viewportWidth)
        return cached;

    ChatLayout::LayoutContext ctx = buildLayoutContext(option, index);
    ctx.rowWidth = viewportWidth;
    ctx.rowTop = 0;

    ChatLayout::MessageLayout layout = ChatLayout::calculateMessageLayout(ctx);

    QSize size(viewportWidth, layout.totalHeight);

    auto model = const_cast<QAbstractItemModel *>(index.model());
    const auto prevSize = index.data(ChatModel::CachedSizeRole).toSize();
    if (size != prevSize)
        model->setData(index, size, ChatModel::CachedSizeRole);

    return size;
}

} // namespace UI
} // namespace Acheron
