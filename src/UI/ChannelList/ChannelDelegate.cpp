#include "ChannelDelegate.hpp"

#include "ChannelNode.hpp"

namespace Acheron {
namespace UI {
ChannelDelegate::ChannelDelegate(QObject *parent) : QStyledItemDelegate(parent) { }

void ChannelDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
{
    painter->save();

    ChannelNode *node = static_cast<ChannelNode *>(index.internalPointer());
    if (!node) {
        painter->restore();
        return;
    }

    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    painter->fillRect(option.rect, option.palette.base());

    constexpr int iconSize = 24;

    if (node->type == ChannelNode::Type::Server) {
        QPixmap icon = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
        QRect iconRect = QRect(option.rect.left() + (iconSize - iconSize) / 2,
                               option.rect.top() + (option.rect.height() - iconSize) / 2, iconSize,
                               iconSize);
        painter->drawPixmap(iconRect, icon);
    }

    QString text = index.data(Qt::DisplayRole).toString();
    QRect textRect = option.rect.adjusted(iconSize, 0, -iconSize, 0);
    painter->setPen(option.palette.text().color());
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);

    if (node->type == ChannelNode::Type::Account) {
        painter->fillRect(option.rect, QColor(40, 40, 45));
        painter->setPen(Qt::white);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, node->name);
        painter->restore();
        return;
    } else if (node->type == ChannelNode::Type::Category) {
        int indicatorWidth = 20;
        QRect branchRect = option.rect;
        branchRect.setWidth(indicatorWidth);

        QStyleOptionViewItem branchOpt = option;
        branchOpt.rect = branchRect;

        branchOpt.state |= QStyle::State_Children;

        if (option.state & QStyle::State_Open) {
            branchOpt.state |= QStyle::State_Open;
        }

        QApplication::style()->drawPrimitive(QStyle::PE_IndicatorBranch, &branchOpt, painter,
                                             option.widget);
    }

    int unreadCount = index.data(Qt::UserRole + 1).toInt();
    if (unreadCount > 0) {
        QRect badgeRect(option.rect.right() - 20, option.rect.top() + 4, 16, 16);
        painter->setBrush(Qt::red);
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(badgeRect);

        painter->setPen(Qt::white);
        QFont font("Arial", 8, QFont::Bold);
        painter->setFont(font);
        painter->drawText(badgeRect, Qt::AlignCenter, QString::number(unreadCount));
    }

    painter->restore();
}

QSize ChannelDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize sz = QStyledItemDelegate::sizeHint(option, index);
    return QSize(sz.width(), 24);
}
} // namespace UI
} // namespace Acheron
