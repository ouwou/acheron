#include "StatusIndicator.hpp"

#include <QCoreApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPolygonF>
#include <QtMath>

#include "Core/Theme/Manager.hpp"

namespace Acheron {
namespace UI {

using Core::PresenceBadge;

namespace StatusIndicator {

namespace {

using Core::Theme::Token;

constexpr qreal PhoneWidthRatio = 0.7;
constexpr qreal PhoneHeightRatio = 1.25;

QColor color(const PresenceBadge &badge)
{
    const Core::Theme::Manager &theme = Core::Theme::Manager::instance();

    if (badge.streaming)
        return theme.color(Token::StatusStreaming);

    switch (badge.status) {
    case Discord::StatusType::ONLINE:
        return theme.color(Token::StatusOnline);
    case Discord::StatusType::IDLE:
        return theme.color(Token::StatusIdle);
    case Discord::StatusType::DND:
        return theme.color(Token::StatusDnd);
    default:
        return theme.color(Token::StatusOffline);
    }
}

bool isPhone(const PresenceBadge &badge)
{
    return badge.mobile && !badge.streaming && badge.status == Discord::StatusType::ONLINE;
}

QSizeF badgeSize(int dotSize, const PresenceBadge &badge)
{
    if (isPhone(badge))
        return QSizeF(dotSize * PhoneWidthRatio, dotSize * PhoneHeightRatio);

    return QSizeF(dotSize, dotSize);
}

qreal ringWidth(int dotSize)
{
    return qMax<qreal>(1.0, dotSize * 0.18);
}

QSizeF dotBox(int dotSize)
{
    const qreal ring = ringWidth(dotSize);
    return QSizeF(dotSize + ring * 2, dotSize * PhoneHeightRatio + ring * 2);
}

QPainterPath badgePath(const QRectF &rect, const PresenceBadge &badge)
{
    const qreal size = qMin(rect.width(), rect.height());

    QPainterPath body;
    QPainterPath cutout;

    if (badge.streaming) {
        body.addEllipse(rect);

        const qreal inset = size * 0.28;
        const QPointF center = rect.center();
        QPolygonF triangle;
        triangle << QPointF(center.x() - inset * 0.6, center.y() - inset)
                 << QPointF(center.x() - inset * 0.6, center.y() + inset)
                 << QPointF(center.x() + inset, center.y());
        cutout.addPolygon(triangle);
        cutout.closeSubpath();
    } else if (isPhone(badge)) {
        const qreal radius = rect.width() * 0.3;
        body.addRoundedRect(rect, radius, radius);

        const qreal barWidth = rect.width() * 0.5;
        const qreal barHeight = qMax<qreal>(1.0, rect.height() * 0.08);
        cutout.addRoundedRect(QRectF(rect.center().x() - barWidth / 2.0,
                                     rect.bottom() - rect.height() * 0.22,
                                     barWidth,
                                     barHeight),
                              barHeight / 2.0,
                              barHeight / 2.0);
    } else {
        body.addEllipse(rect);

        switch (badge.status) {
        case Discord::StatusType::IDLE:
            cutout.addEllipse(QPointF(rect.left() + size * 0.25,
                                      rect.top() + size * 0.25),
                              size * 0.375,
                              size * 0.375);
            break;
        case Discord::StatusType::DND:
            cutout.addRoundedRect(QRectF(rect.left() + size * 0.125,
                                         rect.top() + size * 0.375,
                                         size * 0.75,
                                         size * 0.25),
                                  size * 0.125,
                                  size * 0.125);
            break;
        case Discord::StatusType::ONLINE:
            break;
        default:
            cutout.addEllipse(rect.center(), size * 0.25, size * 0.25);
            break;
        }
    }

    if (cutout.isEmpty())
        return body;

    return body.subtracted(cutout);
}

void paintWithRing(QPainter &painter, const QRectF &rect, qreal ring, const PresenceBadge &badge, const QColor &ringColor)
{
    const QRectF outer = rect.adjusted(-ring, -ring, ring, ring);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ringColor);
    painter.drawRoundedRect(outer, outer.width() / 2.0, outer.height() / 2.0);
    painter.fillPath(badgePath(rect, badge), color(badge));
    painter.restore();
}

} // namespace

QString tooltip(const PresenceBadge &badge)
{
    if (badge.streaming)
        return QCoreApplication::translate("StatusIndicator", "Streaming");

    switch (badge.status) {
    case Discord::StatusType::ONLINE:
        return badge.mobile ? QCoreApplication::translate("StatusIndicator", "Online on mobile")
                            : QCoreApplication::translate("StatusIndicator", "Online");
    case Discord::StatusType::IDLE:
        return QCoreApplication::translate("StatusIndicator", "Idle");
    case Discord::StatusType::DND:
        return QCoreApplication::translate("StatusIndicator", "Do Not Disturb");
    case Discord::StatusType::INVISIBLE:
        return QCoreApplication::translate("StatusIndicator", "Invisible");
    default:
        return QCoreApplication::translate("StatusIndicator", "Offline");
    }
}

void paintOnAvatar(QPainter &painter, const QRect &avatarRect, int dotSize, const PresenceBadge &badge, const QColor &ringColor)
{
    if (dotSize <= 0)
        return;

    const QSizeF size = badgeSize(dotSize, badge);
    const QRectF rect(avatarRect.right() - size.width() + 1,
                      avatarRect.bottom() - size.height() + 1,
                      size.width(),
                      size.height());

    paintWithRing(painter, rect, ringWidth(dotSize), badge, ringColor);
}

} // namespace StatusIndicator

StatusDot *attachStatusDot(QWidget *parent, const QRect &avatarRect, int dotSize)
{
    auto *dot = new StatusDot(parent);
    dot->setDotSize(dotSize);
    dot->setFixedSize(dot->sizeHint());
    dot->move(avatarRect.right() + 1 - dot->width(), avatarRect.bottom() + 1 - dot->height());
    dot->raise();
    return dot;
}

void StatusDot::setBadge(const PresenceBadge &newBadge)
{
    badge = newBadge;
    setToolTip(StatusIndicator::tooltip(badge));
    update();
}

void StatusDot::setDotSize(int size)
{
    dotSize = size;
    updateGeometry();
    update();
}

QSize StatusDot::sizeHint() const
{
    const QSizeF box = StatusIndicator::dotBox(dotSize);
    return QSize(qCeil(box.width()), qCeil(box.height()));
}

void StatusDot::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    const QSizeF size = StatusIndicator::badgeSize(dotSize, badge);
    const qreal ring = StatusIndicator::ringWidth(dotSize);
    const QRectF rect(width() - ring - size.width(),
                      height() - ring - size.height(),
                      size.width(),
                      size.height());

    StatusIndicator::paintWithRing(painter, rect, ring, badge, palette().color(QPalette::Window));
}

} // namespace UI
} // namespace Acheron
