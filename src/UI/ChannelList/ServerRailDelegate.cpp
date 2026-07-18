#include "ServerRailDelegate.hpp"

#include "ServerRailModel.hpp"

#include <QPainter>
#include <QPainterPath>

namespace Acheron {
namespace UI {

namespace {

constexpr int IconSize = 38;
constexpr int FolderGroupAlpha = 64;

QString initialsFor(const QString &name)
{
    const QStringList parts = name.split(' ', Qt::SkipEmptyParts);
    QString out;
    for (const auto &p : parts)
        out += p.left(1);
    if (out.isEmpty())
        out = name.left(1);
    return out.toUpper();
}

QColor monogramColor(const QString &name, const QPalette &palette)
{
    int h, s, v;
    palette.color(QPalette::Highlight).getHsv(&h, &s, &v);
    if (h < 0)
        h = 0;
    const uint hash = qHash(name);
    h = (h + static_cast<int>(hash % 90) - 45 + 360) % 360;
    v = qBound(70, v + static_cast<int>((hash / 90) % 70) - 25, 200);
    return QColor::fromHsv(h, qMax(60, s), v);
}

void drawRoundedPixmap(QPainter *painter, const QRect &rect, const QPixmap &pixmap, qreal radius)
{
    painter->save();
    QPainterPath clip;
    clip.addRoundedRect(rect, radius, radius);
    painter->setClipPath(clip);
    painter->drawPixmap(rect, pixmap);
    painter->restore();
}

void drawMonogram(QPainter *painter, const QRect &rect, const QString &text, const QColor &bg, qreal radius)
{
    painter->setPen(Qt::NoPen);
    painter->setBrush(bg);
    painter->drawRoundedRect(rect, radius, radius);

    QFont font = painter->font();
    font.setBold(true);

    const QString fitText = text.left(8);
    const qreal avail = rect.width();
    const int px = qMax(1, static_cast<int>(rect.height() * 0.4));
    font.setPixelSize(px);
    const qreal w = QFontMetrics(font).horizontalAdvance(fitText);
    if (w > avail)
        font.setPixelSize(qMax(1, static_cast<int>(px * avail / w)));

    painter->setFont(font);
    painter->setPen(Qt::white);
    painter->drawText(rect, Qt::AlignCenter, text);
}

QColor folderBaseColor(const QVariant &folderColorRole, const QPalette &palette)
{
    if (folderColorRole.isValid())
        return QColor::fromRgb(folderColorRole.toUInt());
    return palette.color(QPalette::Highlight);
}

void drawFolderGlyph(QPainter *painter, const QRect &iconRect, const QColor &color)
{
    const qreal pad = iconRect.width() * 0.16;
    QRectF r(iconRect.left() + pad, iconRect.top() + pad,
             iconRect.width() - 2 * pad, iconRect.height() - 2 * pad);
    const qreal w = r.width();
    const qreal h = r.height();
    const qreal round = h * 0.14;

    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawRoundedRect(QRectF(r.left(), r.top(), w * 0.46, h * 0.42), round, round);
    painter->drawRoundedRect(QRectF(r.left(), r.top() + h * 0.22, w, h * 0.66), round, round);
    painter->restore();
}

void drawLeftIndicator(QPainter *painter, const QStyleOptionViewItem &option, int iconSize, bool selected, bool unread)
{
    int pillH = 0;
    if (selected)
        pillH = iconSize;
    else if (unread)
        pillH = 8;
    if (pillH == 0)
        return;

    constexpr int pillW = 4;
    int pillX = option.rect.left();
    int pillY = option.rect.top() + (option.rect.height() - pillH) / 2;

    painter->setPen(Qt::NoPen);
    painter->setBrush(option.palette.brightText().color());
    painter->drawRoundedRect(QRect(pillX - pillW, pillY, pillW * 2, pillH), pillW, pillW);
}

void drawMentionBadge(QPainter *painter, const QStyleOptionViewItem &option, const QRect &iconRect, int count)
{
    const QString text = count > 99 ? QStringLiteral("99+") : QString::number(count);

    QFont font = option.font;
    font.setBold(true);
    font.setPixelSize(11);
    QFontMetrics fm(font);

    int badgeH = 18;
    int badgeW = qMax(badgeH, fm.horizontalAdvance(text) + 8);
    QRect badge(iconRect.right() - badgeW + 6, iconRect.bottom() - badgeH + 6, badgeW, badgeH);

    painter->setPen(Qt::NoPen);
    painter->setBrush(option.palette.base().color());
    painter->drawRoundedRect(badge.adjusted(-2, -2, 2, 2), (badgeH + 4) / 2.0, (badgeH + 4) / 2.0);

    painter->setBrush(option.palette.highlight().color());
    painter->drawRoundedRect(badge, badgeH / 2.0, badgeH / 2.0);

    painter->setFont(font);
    painter->setPen(option.palette.highlightedText().color());
    painter->drawText(badge, Qt::AlignCenter, text);
}

void fillGroupBackground(QPainter *painter, const QRectF &rect, const QColor &color, bool roundTop, bool roundBottom, qreal radius)
{
    const qreal r = radius;
    QPainterPath path;

    path.moveTo(rect.left(), roundTop ? rect.top() + r : rect.top());
    if (roundTop)
        path.quadTo(rect.left(), rect.top(), rect.left() + r, rect.top());
    path.lineTo(roundTop ? rect.right() - r : rect.right(), rect.top());
    if (roundTop)
        path.quadTo(rect.right(), rect.top(), rect.right(), rect.top() + r);
    path.lineTo(rect.right(), roundBottom ? rect.bottom() - r : rect.bottom());
    if (roundBottom)
        path.quadTo(rect.right(), rect.bottom(), rect.right() - r, rect.bottom());
    path.lineTo(roundBottom ? rect.left() + r : rect.left(), rect.bottom());
    if (roundBottom)
        path.quadTo(rect.left(), rect.bottom(), rect.left(), rect.bottom() - r);
    path.closeSubpath();

    painter->setPen(Qt::NoPen);
    painter->fillPath(path, color);
}

} // namespace

ServerRailDelegate::ServerRailDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

void ServerRailDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    painter->fillRect(option.rect, option.palette.base());

    const auto kind = static_cast<ServerRailModel::Kind>(index.data(ServerRailModel::KindRole).toInt());
    const bool isFirst = index.data(ServerRailModel::IsFirstOfAccountRole).toBool();
    const bool selected = index.data(ServerRailModel::IsSelectedRole).toBool();
    const bool muted = index.data(ServerRailModel::IsMutedRole).toBool();
    const bool unread = index.data(ServerRailModel::IsUnreadRole).toBool() && !muted;
    const int mentions = index.data(ServerRailModel::MentionCountRole).toInt();
    const QString name = index.data(Qt::DisplayRole).toString();
    const bool inFolderGroup = index.data(ServerRailModel::InFolderGroupRole).toBool();

    // separator above each account section
    if (isFirst && index.row() > 0) {
        QColor sep = option.palette.mid().color();
        sep.setAlpha(90);
        painter->setPen(QPen(sep, 1));
        painter->drawLine(option.rect.left() + 14, option.rect.top(),
                          option.rect.right() - 14, option.rect.top());
    }

    const int iconSize = IconSize;
    const int x = option.rect.left() + (option.rect.width() - iconSize) / 2;
    const int y = option.rect.top() + (option.rect.height() - iconSize) / 2;
    const QRect iconRect(x, y, iconSize, iconSize);

    qreal radius = (kind == ServerRailModel::Kind::AccountHome) ? iconSize / 2.0 : 16.0;
    if (selected && kind != ServerRailModel::Kind::AccountHome)
        radius = iconSize * 0.30;

    const bool isFolder = kind == ServerRailModel::Kind::Folder;
    if (isFolder || inFolderGroup) {
        const QModelIndex next = index.siblingAtRow(index.row() + 1);
        const bool nextIsChild =
                next.isValid() && next.data(ServerRailModel::InFolderGroupRole).toBool() &&
                static_cast<ServerRailModel::Kind>(next.data(ServerRailModel::KindRole).toInt()) == ServerRailModel::Kind::Server;
        const bool isGroupTop = isFolder;
        const bool isGroupBottom = !nextIsChild;

        QColor groupColor = folderBaseColor(index.data(ServerRailModel::FolderColorRole), option.palette);
        groupColor.setAlpha(FolderGroupAlpha);

        const qreal vInset = (option.rect.height() - iconSize) / 2.0;
        const qreal top = option.rect.top() + (isGroupTop ? vInset : 0.0);
        const qreal bottom = option.rect.top() + option.rect.height() - (isGroupBottom ? vInset : 0.0);
        QRectF bg(iconRect.left(), top, iconRect.width(), bottom - top);
        fillGroupBackground(painter, bg, groupColor, isGroupTop, isGroupBottom, 16.0);
    }

    drawLeftIndicator(painter, option, iconSize, selected, unread);

    if (isFolder) {
        const QVariant colorVar = index.data(ServerRailModel::FolderColorRole);
        if (index.data(ServerRailModel::IsExpandedRole).toBool()) {
            drawFolderGlyph(painter, iconRect, folderBaseColor(colorVar, option.palette));
        } else {
            const QVariantList icons = index.data(ServerRailModel::FolderChildIconsRole).toList();
            constexpr int pad = 6;
            constexpr int gap = 3;
            const int cell = (iconSize - pad * 2 - gap) / 2;
            const qreal cellRadius = cell * 0.3;
            for (int i = 0; i < icons.size() && i < 4; ++i) {
                int cx = iconRect.left() + pad + (i % 2) * (cell + gap);
                int cy = iconRect.top() + pad + (i / 2) * (cell + gap);
                QRect cellRect(cx, cy, cell, cell);
                QPixmap pm = icons[i].value<QPixmap>();
                if (!pm.isNull()) {
                    drawRoundedPixmap(painter, cellRect, pm, cellRadius);
                } else {
                    QColor blank = Qt::white;
                    blank.setAlpha(40);
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(blank);
                    painter->drawRoundedRect(cellRect, cellRadius, cellRadius);
                }
            }
        }
    } else {
        QPixmap icon = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
        if (!icon.isNull())
            drawRoundedPixmap(painter, iconRect, icon, radius);
        else {
            QColor bg = monogramColor(name, option.palette);
            if (!selected)
                bg.setAlpha(110);
            drawMonogram(painter, iconRect, initialsFor(name), bg, radius);
        }
    }

    if (mentions > 0)
        drawMentionBadge(painter, option, iconRect, mentions);

    painter->restore();
}

QSize ServerRailDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return QSize(RailWidth, ItemHeight);
}

} // namespace UI
} // namespace Acheron
