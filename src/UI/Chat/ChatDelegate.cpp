#include "ChatDelegate.hpp"

#include "ChatModel.hpp"
#include "ChatLayout.hpp"
#include "ChatView.hpp"

namespace Acheron {
namespace UI {

void ChatDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
{
    painter->save();

    const QString content = index.data(ChatModel::ContentRole).toString();
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
        if (!avatar.isNull()) {
            painter->drawPixmap(avatarRect, avatar);
        }

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

            if (endChar == -1) {
                cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
            } else {
                cursor.setPosition(endChar, QTextCursor::KeepAnchor);
            }

            QAbstractTextDocumentLayout::Selection sel;
            sel.cursor = cursor;
            sel.format.setBackground(option.palette.highlight());
            sel.format.setForeground(option.palette.highlightedText());
            ctx.selections.append(sel);
        }
    }

    doc.documentLayout()->draw(painter, ctx);

    // reset translation for attachment rendering
    painter->restore();
    painter->save();

    // render attachments below text
    QList<AttachmentData> attachments =
            index.data(ChatModel::AttachmentsRole).value<QList<AttachmentData>>();
    if (!attachments.isEmpty()) {
        int realTextHeight = int(std::ceil(doc.size().height()));
        int attachmentTop = textRect.top() + realTextHeight + ChatLayout::padding();

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

                QRect imgRect = cell.rect.translated(textRect.left(), attachmentTop);

                if (!att.pixmap.isNull()) {
                    if (isSingleImage) {
                        QRect actualRect(imgRect.x(), imgRect.y(), att.displaySize.width(),
                                         att.displaySize.height());
                        painter->drawPixmap(actualRect, att.pixmap);
                    } else {
                        QSize pixSize = att.pixmap.size() / att.pixmap.devicePixelRatio();
                        QRect sourceRect;

                        qreal targetAspect = qreal(imgRect.width()) / imgRect.height();
                        qreal pixAspect = qreal(pixSize.width()) / pixSize.height();

                        // crop to fit
                        if (pixAspect > targetAspect) {
                            int cropWidth = qRound(pixSize.height() * targetAspect);
                            int cropX = (pixSize.width() - cropWidth) / 2;
                            sourceRect = QRect(cropX, 0, cropWidth, pixSize.height());
                        } else {
                            int cropHeight = qRound(pixSize.width() / targetAspect);
                            int cropY = (pixSize.height() - cropHeight) / 2;
                            sourceRect = QRect(0, cropY, pixSize.width(), cropHeight);
                        }

                        qreal dpr = att.pixmap.devicePixelRatio();
                        QRect physicalSourceRect(qRound(sourceRect.x() * dpr),
                                                 qRound(sourceRect.y() * dpr),
                                                 qRound(sourceRect.width() * dpr),
                                                 qRound(sourceRect.height() * dpr));

                        painter->drawPixmap(imgRect, att.pixmap, physicalSourceRect);
                    }
                } else {
                    painter->fillRect(imgRect, QColor(60, 60, 60));
                    painter->setPen(option.palette.text().color());
                    painter->drawText(imgRect, Qt::AlignCenter, "Loading...");
                }
            }

            if (isSingleImage && !images.isEmpty())
                attachmentTop += images[0].displaySize.height() + ChatLayout::padding();
            else if (!images.isEmpty())
                attachmentTop += grid.totalHeight + ChatLayout::padding();
        }

        constexpr int fileAttachmentHeight = 48;
        constexpr int fileAttachmentPadding = 8;
        constexpr int maxAttachmentWidth = 400;
        int fileWidth = std::min(textRect.width(), maxAttachmentWidth);

        for (const auto &att : files) {
            QRect fileRect(textRect.left(), attachmentTop, fileWidth, fileAttachmentHeight);

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

            attachmentTop = fileRect.bottom() + ChatLayout::padding();
        }
    }

    // render embeds below attachments
    QList<EmbedData> embeds = index.data(ChatModel::EmbedsRole).value<QList<EmbedData>>();
    if (!embeds.isEmpty()) {
        int realTextHeight = int(std::ceil(doc.size().height()));
        int embedTop = textRect.top() + realTextHeight + ChatLayout::padding();

        // adjust for attachments if any
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
                embedTop += firstImageSize.height() + ChatLayout::padding();
            else if (imageCount > 1) {
                ChatLayout::AttachmentGridLayout grid =
                        ChatLayout::calculateAttachmentGrid(imageCount, textRect.width());
                embedTop += grid.totalHeight + ChatLayout::padding();
            }

            constexpr int fileAttachmentHeight = 48;
            embedTop += fileCount * (fileAttachmentHeight + ChatLayout::padding());
        }

        constexpr int embedMaxWidth = 400;
        constexpr int embedBorderWidth = 4;
        constexpr int embedPadding = 12;
        constexpr int thumbnailSize = 80;
        constexpr int authorIconSize = 24;
        constexpr int footerIconSize = 16;
        constexpr int fieldSpacing = 8;

        for (const auto &embed : embeds) {
            int embedWidth = std::min(textRect.width(), embedMaxWidth);
            int contentWidth = embedWidth - embedBorderWidth - embedPadding * 2;

            bool hasThumbnail = !embed.thumbnail.isNull() ||
                                (!embed.videoThumbnail.isNull() && embed.images.isEmpty());
            if (hasThumbnail)
                contentWidth -= (thumbnailSize + embedPadding);

            int currentY = 0;

            // do height first for the background
            int embedHeight = embedPadding;

            if (!embed.providerName.isEmpty()) {
                QFont providerFont = option.font;
                providerFont.setPointSize(providerFont.pointSize() - 2);
                QFontMetrics providerFm(providerFont);
                embedHeight += providerFm.height() + 4;
            }

            if (!embed.authorName.isEmpty()) {
                QFont authorFont = option.font;
                authorFont.setPointSize(authorFont.pointSize() - 1);
                authorFont.setBold(true);
                QFontMetrics authorFm(authorFont);
                embedHeight += std::max(authorIconSize, authorFm.height()) + 4;
            }

            if (!embed.title.isEmpty()) {
                QFont titleFont = option.font;
                titleFont.setBold(true);
                QFontMetrics titleFm(titleFont);
                embedHeight += titleFm.height() + 4;
            }

            int descriptionHeight = 0;
            if (!embed.description.isEmpty()) {
                QTextDocument descDoc;
                descDoc.setDefaultFont(option.font);
                descDoc.setTextWidth(contentWidth);
                descDoc.setPlainText(embed.description);
                descriptionHeight = int(std::ceil(descDoc.size().height()));
                embedHeight += descriptionHeight + 8;
            }

            if (!embed.fields.isEmpty()) {
                QFont fieldNameFont = option.font;
                fieldNameFont.setBold(true);
                QFontMetrics fieldNameFm(fieldNameFont);
                int fieldWidth = (contentWidth - 2 * fieldSpacing) / 3;

                int fieldsInRow = 0;
                int maxRowHeight = 0;
                for (const auto &field : embed.fields) {
                    QTextDocument valueDoc;
                    valueDoc.setDefaultFont(option.font);
                    valueDoc.setTextWidth(field.isInline ? fieldWidth : contentWidth);
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

            if (!embed.images.isEmpty()) {
                if (embed.images.size() == 1) {
                    const auto &img = embed.images[0];
                    if (!img.pixmap.isNull()) {
                        QSize actualSize =
                                img.pixmap.size().scaled(img.displaySize, Qt::KeepAspectRatio);
                        embedHeight += actualSize.height();
                    }
                } else {
                    ChatLayout::AttachmentGridLayout grid =
                            ChatLayout::calculateAttachmentGrid(embed.images.size(), contentWidth);
                    embedHeight += grid.totalHeight;
                }
            } else if (!embed.videoThumbnail.isNull() && embed.thumbnail.isNull()) {
                QSize actualSize = embed.videoThumbnail.size().scaled(embed.videoThumbnailSize,
                                                                      Qt::KeepAspectRatio);
                embedHeight += actualSize.height();
            }

            if (!embed.footerText.isEmpty()) {
                QFont footerFont = option.font;
                footerFont.setPointSize(footerFont.pointSize() - 2);
                QFontMetrics footerFm(footerFont);
                embedHeight += std::max(footerIconSize, footerFm.height()) + 4;
            }

            if (hasThumbnail)
                embedHeight = std::max(embedHeight, int(embedPadding * 2 + thumbnailSize));

            // embedHeight += embedPadding;

            QRect embedRect(textRect.left(), embedTop, embedWidth, embedHeight);
            QColor bgColor = option.palette.base().color().darker(110);
            painter->fillRect(embedRect, bgColor);

            // left thing
            QRect borderRect(embedRect.left(), embedRect.top(), embedBorderWidth,
                             embedRect.height());
            painter->fillRect(borderRect, embed.color);

            int contentLeft = embedRect.left() + embedBorderWidth + embedPadding;
            int contentTop = embedRect.top() + embedPadding;
            currentY = contentTop;

            if (hasThumbnail) {
                QPixmap thumb = !embed.thumbnail.isNull() ? embed.thumbnail : embed.videoThumbnail;
                QSize thumbDisplaySize =
                        !embed.thumbnail.isNull() ? embed.thumbnailSize : embed.videoThumbnailSize;
                if (!thumb.isNull()) {
                    int thumbX = contentLeft + contentWidth + embedPadding;
                    QPixmap scaledThumb = thumb.scaled(thumbDisplaySize, Qt::KeepAspectRatio,
                                                       Qt::SmoothTransformation);
                    painter->drawPixmap(thumbX, currentY, scaledThumb);
                }
            }

            if (!embed.providerName.isEmpty()) {
                QFont providerFont = option.font;
                providerFont.setPointSize(providerFont.pointSize() - 2);
                painter->setFont(providerFont);
                painter->setPen(option.palette.placeholderText().color());
                QFontMetrics providerFm(providerFont);
                painter->drawText(contentLeft, currentY + providerFm.ascent(), embed.providerName);
                currentY += providerFm.height() + 4;
            }

            if (!embed.authorName.isEmpty()) {
                int authorX = contentLeft;
                if (!embed.authorIcon.isNull()) {
                    QRect iconRect(authorX, currentY, authorIconSize, authorIconSize);
                    painter->drawPixmap(iconRect, embed.authorIcon);
                    authorX += authorIconSize + 6;
                }
                QFont authorFont = option.font;
                authorFont.setPointSize(authorFont.pointSize() - 1);
                authorFont.setBold(true);
                painter->setFont(authorFont);
                painter->setPen(option.palette.text().color());
                QFontMetrics authorFm(authorFont);
                int textY = currentY + (authorIconSize - authorFm.height()) / 2 + authorFm.ascent();
                painter->drawText(authorX, textY, embed.authorName);
                currentY += std::max(authorIconSize, authorFm.height()) + 4;
            }

            if (!embed.title.isEmpty()) {
                QFont titleFont = option.font;
                titleFont.setBold(true);
                painter->setFont(titleFont);
                if (!embed.url.isEmpty())
                    painter->setPen(QColor(0, 168, 252)); // todo change
                else
                    painter->setPen(option.palette.text().color());
                QFontMetrics titleFm(titleFont);
                QString elidedTitle = titleFm.elidedText(embed.title, Qt::ElideRight, contentWidth);
                painter->drawText(contentLeft, currentY + titleFm.ascent(), elidedTitle);
                currentY += titleFm.height() + 4;
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

                currentY += int(std::ceil(descDoc.size().height())) + 8;
            }

            if (!embed.fields.isEmpty()) {
                QFont fieldNameFont = option.font;
                fieldNameFont.setBold(true);
                QFontMetrics fieldNameFm(fieldNameFont);

                int fieldWidth = (contentWidth - 2 * fieldSpacing) / 3;
                int fieldX = contentLeft;
                int fieldsInRow = 0;
                int rowStartY = currentY;
                int maxRowHeight = 0;

                for (const auto &field : embed.fields) {
                    if (!field.isInline) {
                        if (fieldsInRow > 0) {
                            currentY = rowStartY + maxRowHeight + fieldSpacing;
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
                        currentY += fieldHeight + fieldSpacing;
                        rowStartY = currentY;
                    } else {
                        if (fieldsInRow >= 3) {
                            currentY = rowStartY + maxRowHeight + fieldSpacing;
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
                        fieldX += fieldWidth + fieldSpacing;
                        fieldsInRow++;
                    }
                }

                if (fieldsInRow > 0)
                    currentY = rowStartY + maxRowHeight + fieldSpacing;
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
                            QSize pixSize = img.pixmap.size() / img.pixmap.devicePixelRatio();
                            QRect sourceRect;

                            qreal targetAspect = qreal(imgRect.width()) / imgRect.height();
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

                            qreal dpr = img.pixmap.devicePixelRatio();
                            QRect physicalSourceRect(qRound(sourceRect.x() * dpr),
                                                     qRound(sourceRect.y() * dpr),
                                                     qRound(sourceRect.width() * dpr),
                                                     qRound(sourceRect.height() * dpr));

                            painter->drawPixmap(imgRect, img.pixmap, physicalSourceRect);
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
                    QRect iconRect(footerX, currentY, footerIconSize, footerIconSize);
                    painter->drawPixmap(iconRect, embed.footerIcon);
                    footerX += footerIconSize + 6;
                }
                QFont footerFont = option.font;
                footerFont.setPointSize(footerFont.pointSize() - 2);
                painter->setFont(footerFont);
                painter->setPen(option.palette.placeholderText().color());
                QFontMetrics footerFm(footerFont);

                QString footerText = embed.footerText;
                if (embed.timestamp.isValid())
                    footerText += " • " + embed.timestamp.toString("MMM d, yyyy h:mm AP");

                int textY = currentY + (footerIconSize - footerFm.height()) / 2 + footerFm.ascent();
                painter->drawText(footerX, textY, footerText);
            }

            embedTop += embedHeight + ChatLayout::padding();
        }
    }

    painter->restore();

#if 0
    // correct height calculation
    {
        int realHeight = int(std::ceil(doc.size().height()));

        int requiredHeight = 0;
        QFontMetrics fm(option.font);
        constexpr int pad = ChatLayout::padding();
        constexpr int aSz = ChatLayout::avatarSize();

        if (showHeader) {
            int contentHeight = pad + fm.height() + realHeight + pad;
            int minHeight = pad + aSz + pad;
            requiredHeight = std::max(contentHeight, minHeight);
        } else {
            requiredHeight = realHeight + pad + 1;
        }

        if (hasSeparator) {
            requiredHeight += ChatLayout::separatorHeight();
        }

        // add height for attachments
        QList<AttachmentData> atts =
                index.data(ChatModel::AttachmentsRole).value<QList<AttachmentData>>();
        if (!atts.isEmpty()) {
            int imageCount = 0;
            int fileCount = 0;
            QSize firstImageSize;
            for (const auto &att : atts) {
                if (att.isImage) {
                    if (imageCount == 0)
                        firstImageSize = att.displaySize;
                    imageCount++;
                } else {
                    fileCount++;
                }
            }

            if (imageCount == 1) {
                requiredHeight += firstImageSize.height() + pad;
            } else if (imageCount > 1) {
                ChatLayout::AttachmentGridLayout grid = ChatLayout::calculateAttachmentGrid(
                        imageCount, option.rect.width() - pad - aSz - pad - pad);
                requiredHeight += grid.totalHeight + pad;
            }

            constexpr int fileAttachmentHeight = 48;
            requiredHeight += fileCount * (fileAttachmentHeight + pad);
        }

        // add height for embeds
        QList<EmbedData> embs = index.data(ChatModel::EmbedsRole).value<QList<EmbedData>>();
        if (!embs.isEmpty()) {
            constexpr int embedMaxWidth = 400;
            constexpr int embedBorderWidth = 4;
            constexpr int embedPadding = 12;
            constexpr int thumbnailSize = 80;
            constexpr int authorIconSize = 24;
            constexpr int footerIconSize = 16;
            constexpr int fieldSpacing = 8;

            int textWidth = option.rect.width() - pad - aSz - pad - pad;

            for (const auto &embed : embs) {
                int embedWidth = std::min(textWidth, embedMaxWidth);
                int contentWidth = embedWidth - embedBorderWidth - embedPadding * 2;

                bool hasThumbnail = !embed.thumbnail.isNull() ||
                                    (!embed.videoThumbnail.isNull() && embed.images.isEmpty());
                if (hasThumbnail)
                    contentWidth -= (thumbnailSize + embedPadding);

                int embedHeight = embedPadding;

                if (!embed.providerName.isEmpty()) {
                    QFont providerFont = option.font;
                    providerFont.setPointSize(providerFont.pointSize() - 2);
                    QFontMetrics providerFm(providerFont);
                    embedHeight += providerFm.height() + 4;
                }

                if (!embed.authorName.isEmpty()) {
                    QFont authorFont = option.font;
                    authorFont.setPointSize(authorFont.pointSize() - 1);
                    authorFont.setBold(true);
                    QFontMetrics authorFm(authorFont);
                    embedHeight += std::max(authorIconSize, authorFm.height()) + 4;
                }

                if (!embed.title.isEmpty()) {
                    QFont titleFont = option.font;
                    titleFont.setBold(true);
                    QFontMetrics titleFm(titleFont);
                    embedHeight += titleFm.height() + 4;
                }

                if (!embed.description.isEmpty()) {
                    QTextDocument descDoc;
                    descDoc.setDefaultFont(option.font);
                    descDoc.setTextWidth(contentWidth);
                    descDoc.setPlainText(embed.description);
                    embedHeight += int(std::ceil(descDoc.size().height())) + 8;
                }

                if (!embed.fields.isEmpty()) {
                    QFont fieldNameFont = option.font;
                    fieldNameFont.setBold(true);
                    QFontMetrics fieldNameFm(fieldNameFont);
                    int fieldWidthCalc = (contentWidth - 2 * fieldSpacing) / 3;

                    int fieldsInRow = 0;
                    int maxRowHeight = 0;
                    for (const auto &field : embed.fields) {
                        QTextDocument valueDoc;
                        valueDoc.setDefaultFont(option.font);
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

                if (!embed.images.isEmpty()) {
                    if (embed.images.size() == 1) {
                        const auto &img = embed.images[0];
                        if (!img.pixmap.isNull()) {
                            QSize actualSize = img.pixmap.size().scaled(img.displaySize, Qt::KeepAspectRatio);
                            embedHeight += actualSize.height() + embedPadding;
                        }
                    } else {
                        ChatLayout::AttachmentGridLayout grid =
                                ChatLayout::calculateAttachmentGrid(embed.images.size(), contentWidth);
                        embedHeight += grid.totalHeight + embedPadding;
                    }
                } else if (!embed.videoThumbnail.isNull() && embed.thumbnail.isNull()) {
                    QSize actualSize = embed.videoThumbnail.size().scaled(embed.videoThumbnailSize,
                                                                          Qt::KeepAspectRatio);
                    embedHeight += actualSize.height() + embedPadding;
                }

                if (!embed.footerText.isEmpty()) {
                    QFont footerFont = option.font;
                    footerFont.setPointSize(footerFont.pointSize() - 2);
                    QFontMetrics footerFm(footerFont);
                    embedHeight += std::max(footerIconSize, footerFm.height()) + 4;
                }

                if (hasThumbnail)
                    embedHeight = std::max(embedHeight, int(embedPadding * 2 + thumbnailSize));

                embedHeight += embedPadding;
                requiredHeight += embedHeight + pad;
            }
        }

        if (option.rect.height() != requiredHeight) {

            auto model = const_cast<QAbstractItemModel *>(index.model());
            QSize exactSize(option.rect.width(), requiredHeight);

            QSize currentCache = index.data(ChatModel::CachedSizeRole).toSize();

            if (currentCache != exactSize) {
                model->setData(index, exactSize, ChatModel::CachedSizeRole);

                QMetaObject::invokeMethod(model, "triggerResize", Qt::QueuedConnection,
                                          Q_ARG(int, index.row()));
            }
        }
    }
#endif
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
    if (cached.isValid() && cached.width() == viewportWidth) {
        return cached;
    }

    const QString content = index.data(ChatModel::ContentRole).toString();
    const bool showHeader = index.data(ChatModel::ShowHeaderRole).toBool();
    const bool hasSeparator = index.data(ChatModel::DateSeparatorRole).toBool();

    QFontMetrics fm(option.font);
    constexpr int pad = ChatLayout::padding();
    constexpr int aSz = ChatLayout::avatarSize();

    int textLeft = pad + aSz + pad;
    int textWidth = viewportWidth - textLeft - pad;
    if (textWidth < 10)
        textWidth = 10;

    int approxCharWidth = fm.averageCharWidth();
    int totalTextPixels = content.length() * approxCharWidth;
    int approxLines = (totalTextPixels / textWidth) + 1;

    int textHeight = approxLines * fm.lineSpacing();

    int totalHeight = 0;
    if (showHeader) {
        int contentHeight = pad + fm.height() + textHeight + pad;
        int minHeight = pad + aSz + pad;
        totalHeight = std::max(contentHeight, minHeight);
    } else {
        totalHeight = textHeight + pad + 1;
    }

    if (hasSeparator) {
        totalHeight += ChatLayout::separatorHeight();
    }

    // add height for attachments
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

        if (imageCount == 1) {
            totalHeight += firstImageSize.height() + pad;
        } else if (imageCount > 1) {
            ChatLayout::AttachmentGridLayout grid =
                    ChatLayout::calculateAttachmentGrid(imageCount, textWidth);
            totalHeight += grid.totalHeight + pad;
        }

        constexpr int fileAttachmentHeight = 48;
        totalHeight += fileCount * (fileAttachmentHeight + pad);
    }

    QList<EmbedData> embeds = index.data(ChatModel::EmbedsRole).value<QList<EmbedData>>();
    if (!embeds.isEmpty()) {
        constexpr int embedMaxWidth = 400;
        constexpr int embedBorderWidth = 4;
        constexpr int embedPadding = 12;
        constexpr int thumbnailSize = 80;
        constexpr int authorIconSize = 24;
        constexpr int footerIconSize = 16;
        constexpr int fieldSpacing = 8;

        for (const auto &embed : embeds) {
            int embedWidth = std::min(textWidth, embedMaxWidth);
            int contentWidth = embedWidth - embedBorderWidth - embedPadding * 2;

            bool hasThumbnail = !embed.thumbnail.isNull() ||
                                (!embed.videoThumbnail.isNull() && embed.images.isEmpty());
            if (hasThumbnail)
                contentWidth -= (thumbnailSize + embedPadding);

            int embedHeight = embedPadding;

            if (!embed.providerName.isEmpty()) {
                QFont providerFont = option.font;
                providerFont.setPointSize(providerFont.pointSize() - 2);
                QFontMetrics providerFm(providerFont);
                embedHeight += providerFm.height() + 4;
            }

            if (!embed.authorName.isEmpty()) {
                QFont authorFont = option.font;
                authorFont.setPointSize(authorFont.pointSize() - 1);
                authorFont.setBold(true);
                QFontMetrics authorFm(authorFont);
                embedHeight += std::max(authorIconSize, authorFm.height()) + 4;
            }

            if (!embed.title.isEmpty()) {
                QFont titleFont = option.font;
                titleFont.setBold(true);
                QFontMetrics titleFm(titleFont);
                embedHeight += titleFm.height() + 4;
            }

            if (!embed.description.isEmpty()) {
                QTextDocument descDoc;
                descDoc.setDefaultFont(option.font);
                descDoc.setTextWidth(contentWidth);
                descDoc.setPlainText(embed.description);
                embedHeight += int(std::ceil(descDoc.size().height())) + 8;
            }

            if (!embed.fields.isEmpty()) {
                QFont fieldNameFont = option.font;
                fieldNameFont.setBold(true);
                QFontMetrics fieldNameFm(fieldNameFont);
                int fieldWidthCalc = (contentWidth - 2 * fieldSpacing) / 3;

                int fieldsInRow = 0;
                int maxRowHeight = 0;
                for (const auto &field : embed.fields) {
                    QTextDocument valueDoc;
                    valueDoc.setDefaultFont(option.font);
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

            if (!embed.images.isEmpty()) {
                if (embed.images.size() == 1) {
                    const auto &img = embed.images[0];
                    if (!img.pixmap.isNull()) {
                        QSize actualSize =
                                img.pixmap.size().scaled(img.displaySize, Qt::KeepAspectRatio);
                        embedHeight += actualSize.height();
                    }
                } else {
                    ChatLayout::AttachmentGridLayout grid =
                            ChatLayout::calculateAttachmentGrid(embed.images.size(), contentWidth);
                    embedHeight += grid.totalHeight;
                }
            } else if (!embed.videoThumbnail.isNull() && embed.thumbnail.isNull()) {
                QSize actualSize = embed.videoThumbnail.size().scaled(embed.videoThumbnailSize,
                                                                      Qt::KeepAspectRatio);
                embedHeight += actualSize.height();
            }

            if (!embed.footerText.isEmpty()) {
                QFont footerFont = option.font;
                footerFont.setPointSize(footerFont.pointSize() - 2);
                QFontMetrics footerFm(footerFont);
                embedHeight += std::max(footerIconSize, footerFm.height()) + 4;
            }

            if (hasThumbnail)
                embedHeight = std::max(embedHeight, int(embedPadding * 2 + thumbnailSize));

            // embedHeight += embedPadding;
            totalHeight += embedHeight + pad;
        }
    }

    const auto size = QSize(viewportWidth, totalHeight);
    auto model = const_cast<QAbstractItemModel *>(index.model());
    const auto prevSize = index.data(ChatModel::CachedSizeRole).toSize();
    if (size != prevSize)
        model->setData(index, size, ChatModel::CachedSizeRole);
    return size;
}

} // namespace UI
} // namespace Acheron