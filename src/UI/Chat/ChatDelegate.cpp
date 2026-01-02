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

        painter->setPen(QPen(QColor(60, 60, 60), 1));
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

    painter->restore();

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

    return QSize(viewportWidth, totalHeight);
}

} // namespace UI
} // namespace Acheron