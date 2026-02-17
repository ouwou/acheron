#include "ChannelDelegate.hpp"

#include "ChannelNode.hpp"
#include "ChannelTreeModel.hpp"
#include "ChannelFilterProxyModel.hpp"

namespace Acheron {
namespace UI {
ChannelDelegate::ChannelDelegate(QAbstractProxyModel *proxyModel, QObject *parent)
    : QStyledItemDelegate(parent), proxyModel(proxyModel)
{
}

static void drawBranchIndicator(QPainter *painter, const QStyleOptionViewItem &option, bool expanded)
{
    int indicatorWidth = 20;
    QRect branchRect = option.rect;
    branchRect.setWidth(indicatorWidth);

    QStyleOptionViewItem branchOpt = option;
    branchOpt.rect = branchRect;
    branchOpt.state |= QStyle::State_Children;

    if (expanded)
        branchOpt.state |= QStyle::State_Open;
    else
        branchOpt.state &= ~QStyle::State_Open;

    QApplication::style()->drawPrimitive(QStyle::PE_IndicatorBranch, &branchOpt, painter,
                                         option.widget);
}

static void drawUnreadPill(QPainter *painter, const QStyleOptionViewItem &option)
{
    int pillWidth = 4;
    int pillHeight = 8;
    int pillX = option.rect.left() - pillWidth;
    int pillY = option.rect.top() + (option.rect.height() - pillHeight) / 2;

    painter->setBrush(option.palette.brightText().color());
    painter->setPen(Qt::NoPen);
    painter->drawRoundedRect(QRect(pillX, pillY, pillWidth * 2, pillHeight), pillWidth, pillWidth);
}

static void drawMentionBadge(QPainter *painter, const QStyleOptionViewItem &option, int count)
{
    int badgeHeight = option.fontMetrics.height();
    QString text = QString::number(count);

    QFont font = painter->font();
    font.setWeight(QFont::Bold);
    painter->setFont(font);

    QFontMetrics fm(font);
    int padding = fm.height() / 2;
    int textWidth = fm.horizontalAdvance(text);
    int badgeWidth = qMax(badgeHeight, textWidth + padding);

    QRect badgeRect(option.rect.right() - badgeWidth - 4,
                    option.rect.top() + (option.rect.height() - badgeHeight) / 2,
                    badgeWidth, badgeHeight);

    painter->setBrush(option.palette.highlight().color());
    painter->setPen(Qt::NoPen);
    painter->drawRoundedRect(badgeRect, badgeHeight / 2.0, badgeHeight / 2.0);

    painter->setPen(option.palette.highlightedText().color());
    painter->drawText(badgeRect, Qt::AlignCenter, text);
}

void ChannelDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
{
    painter->save();

    QModelIndex sourceIndex = proxyModel ? proxyModel->mapToSource(index) : index;
    ChannelNode *node = static_cast<ChannelNode *>(sourceIndex.internalPointer());
    if (!node) {
        painter->restore();
        return;
    }

    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    painter->fillRect(option.rect, option.palette.base());

    constexpr int iconSize = 24;
    constexpr int pillMargin = 6;

    // content rect is shifted right to leave room for the unread pill
    QStyleOptionViewItem contentOpt = option;
    contentOpt.rect = option.rect.adjusted(pillMargin, 0, 0, 0);

    if (node->type == ChannelNode::Type::Account) {
        QRect textRect = contentOpt.rect.adjusted(iconSize, 0, -iconSize, 0);
        painter->fillRect(contentOpt.rect, option.palette.alternateBase().color());
        painter->setPen(option.palette.brightText().color());
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, node->name);
        painter->restore();
        return;
    }

    if (node->type == ChannelNode::Type::Folder) {
        if (node->folderColor.has_value())
            painter->fillRect(contentOpt.rect, QColor::fromRgb(node->folderColor.value()));
        else
            painter->fillRect(contentOpt.rect, option.palette.alternateBase());

        painter->setFont(QFont(painter->font().family(), painter->font().pointSize(), QFont::Bold));
        QRect textRect = contentOpt.rect.adjusted(iconSize, 0, -iconSize, 0);
        painter->setPen(option.palette.text().color());
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, node->name);

        drawBranchIndicator(painter, contentOpt, option.state & QStyle::State_Open);

        if (node->isUnread && !node->isMuted)
            drawUnreadPill(painter, option);

        if (node->mentionCount > 0)
            drawMentionBadge(painter, contentOpt, node->mentionCount);

        painter->restore();
        return;
    }

    // draw icon for Server and DMChannel
    if (node->type == ChannelNode::Type::Server || node->type == ChannelNode::Type::DMChannel) {
        QPixmap icon = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
        if (!icon.isNull()) {
            QRect iconRect = QRect(contentOpt.rect.left(),
                                   contentOpt.rect.top() + (contentOpt.rect.height() - iconSize) / 2,
                                   iconSize, iconSize);
            painter->drawPixmap(iconRect, icon);
        }
    }

    // determine text color
    QColor textColor = option.palette.text().color();
    bool isSelected = index.data(ChannelFilterProxyModel::SelectedRole).toBool();
    if (node->type == ChannelNode::Type::Channel || node->type == ChannelNode::Type::VoiceChannel ||
        node->type == ChannelNode::Type::DMChannel) {
        if (node->isMuted)
            textColor = option.palette.text().color().darker(150);
        else if (isSelected || (node->isUnread && node->type != ChannelNode::Type::VoiceChannel))
            textColor = option.palette.brightText().color();
    } else if (node->type == ChannelNode::Type::Server) {
        if (node->isUnread && !node->isMuted)
            textColor = option.palette.brightText().color();
    }

    QRect textRect = contentOpt.rect.adjusted(iconSize, 0, -iconSize, 0);
    painter->setPen(textColor);
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                      index.data(Qt::DisplayRole).toString());

    // branch indicator for categories
    if (node->type == ChannelNode::Type::Category)
        drawBranchIndicator(painter, contentOpt, !node->collapsed);

    // unread pill for channels and servers (draws in the left margin)
    if ((node->type == ChannelNode::Type::Channel || node->type == ChannelNode::Type::DMChannel ||
         node->type == ChannelNode::Type::Server) &&
        node->isUnread && !node->isMuted)
        drawUnreadPill(painter, option);

    // mention badge for channels, servers
    if (node->type != ChannelNode::Type::Category && node->mentionCount > 0 && !node->isMuted)
        drawMentionBadge(painter, contentOpt, node->mentionCount);

    painter->restore();
}

QSize ChannelDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize sz = QStyledItemDelegate::sizeHint(option, index);
    return QSize(sz.width(), 24);
}
} // namespace UI
} // namespace Acheron
