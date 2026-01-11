#include "ChatDelegate.hpp"

#include "ChatModel.hpp"
#include "ChatLayout.hpp"
#include "ChatView.hpp"

#include <algorithm>

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

    ChatLayout::LayoutContext ctx = buildLayoutContext(option, index);
    ChatLayout::MessageLayout layout = ChatLayout::calculateMessageLayout(ctx);

    const QString html = index.data(ChatModel::HtmlRole).toString();
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

    if (layout.showHeader) {
        if (!avatar.isNull())
            painter->drawPixmap(layout.avatarRect, avatar);

        QFont headerFont = option.font;
        headerFont.setBold(true);
        painter->setFont(headerFont);

        QColor headerColor = (option.state & QStyle::State_Selected)
                                     ? option.palette.highlightedText().color()
                                     : option.palette.text().color();
        painter->setPen(headerColor);

        QString header = username + "  " + timestamp.toString("hh:mm");
        painter->drawText(layout.headerRect, Qt::AlignLeft | Qt::AlignTop, header);
    }

    QTextDocument doc;
    ChatLayout::setupDocument(doc, html, option.font, layout.textRect.width());

    painter->translate(layout.textRect.topLeft());

    QAbstractTextDocumentLayout::PaintContext paintCtx;
    QColor textColor = (option.state & QStyle::State_Selected)
                               ? option.palette.highlightedText().color()
                               : option.palette.text().color();
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
            paintCtx.selections.append(sel);
        }
    }

    doc.documentLayout()->draw(painter, paintCtx);

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

        if (!att.pixmap.isNull()) {
            if (isSingleImage)
                painter->drawPixmap(imgLayout.rect, att.pixmap);
            else
                ChatLayout::drawCroppedPixmap(painter, imgLayout.rect, att.pixmap);
        } else {
            painter->fillRect(imgLayout.rect, QColor(60, 60, 60));
            painter->setPen(option.palette.text().color());
            painter->drawText(imgLayout.rect, Qt::AlignCenter, "Loading...");
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
            QColor titleColor =
                    !embed.url.isEmpty() ? QColor(0, 168, 252) : option.palette.text().color();
            painter->setPen(titleColor);

            QTextDocument titleDoc;
            titleDoc.setDefaultFont(titleFont);
            titleDoc.setTextWidth(embedLayout.titleRect.width());
            QString titleHtml = !embed.titleParsed.isEmpty() ? embed.titleParsed : embed.title;
            titleDoc.setHtml(titleHtml);

            painter->save();
            painter->translate(embedLayout.titleRect.topLeft());
            QAbstractTextDocumentLayout::PaintContext titleCtx;
            titleCtx.palette.setColor(QPalette::Text, titleColor);
            titleDoc.documentLayout()->draw(painter, titleCtx);
            painter->restore();
        }

        if (!embed.description.isEmpty() && !embedLayout.descriptionRect.isNull()) {
            QFont descFont = option.font;
            painter->setFont(descFont);
            painter->setPen(option.palette.text().color());

            QTextDocument descDoc;
            descDoc.setDefaultFont(descFont);
            descDoc.setTextWidth(embedLayout.descriptionRect.width());
            QString descHtml = !embed.descriptionParsed.isEmpty() ? embed.descriptionParsed
                                                                  : embed.description;
            descDoc.setHtml(descHtml);

            painter->save();
            painter->translate(embedLayout.descriptionRect.topLeft());
            QAbstractTextDocumentLayout::PaintContext descCtx;
            descCtx.palette.setColor(QPalette::Text, option.palette.text().color());
            descDoc.documentLayout()->draw(painter, descCtx);
            painter->restore();
        }

        QFont fieldNameFont = option.font;
        fieldNameFont.setBold(true);
        QFontMetrics fieldNameFm(fieldNameFont);

        for (const auto &fieldLayout : embedLayout.fieldLayouts) {
            if (fieldLayout.fieldIndex >= embed.fields.size())
                continue;

            const auto &field = embed.fields[fieldLayout.fieldIndex];

            QTextDocument nameDoc;
            nameDoc.setDefaultFont(fieldNameFont);
            nameDoc.setTextWidth(fieldLayout.nameRect.width());
            QString nameHtml = !field.nameParsed.isEmpty() ? field.nameParsed : field.name;
            nameDoc.setHtml(nameHtml);

            painter->save();
            painter->translate(fieldLayout.nameRect.topLeft());
            QAbstractTextDocumentLayout::PaintContext nameCtx;
            nameCtx.palette.setColor(QPalette::Text, option.palette.text().color());
            nameDoc.documentLayout()->draw(painter, nameCtx);
            painter->restore();

            QTextDocument valueDoc;
            valueDoc.setDefaultFont(option.font);
            valueDoc.setTextWidth(fieldLayout.valueRect.width());
            QString valueHtml = !field.valueParsed.isEmpty() ? field.valueParsed : field.value;
            valueDoc.setHtml(valueHtml);

            painter->save();
            painter->translate(fieldLayout.valueRect.topLeft());
            QAbstractTextDocumentLayout::PaintContext valueCtx;
            valueCtx.palette.setColor(QPalette::Text, option.palette.text().color().darker(110));
            valueDoc.documentLayout()->draw(painter, valueCtx);
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
