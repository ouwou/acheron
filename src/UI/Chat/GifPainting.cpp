#include "UI/Chat/GifPainting.hpp"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QPainter>

#include "Core/Theme/Icons.hpp"
#include "UI/Chat/ChatLayout.hpp"

namespace Acheron {
namespace UI {
namespace GifPainting {

namespace {

constexpr int AccessoryInset = 6;
constexpr int AccessoryRadius = 4;
constexpr int AccessoryBackdropAlpha = 153;
constexpr int BadgePaddingX = 5;
constexpr int BadgePaddingY = 1;
constexpr qreal BadgeFontScale = 0.8;
constexpr int StarButtonPx = 28;
constexpr int StarIconPx = 20;
constexpr int SeamlessFitTolerancePx = 2;
const QColor FavoritedStarColor(250, 168, 26);

void drawBackdrop(QPainter *painter, const QRect &rect)
{
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, AccessoryBackdropAlpha));
    painter->drawRoundedRect(rect, AccessoryRadius, AccessoryRadius);
}

} // namespace

void drawBadge(QPainter *painter, const QRect &mediaRect, const QFont &baseFont)
{
    QFont font = baseFont;
    font.setBold(true);
    font.setPointSizeF(baseFont.pointSizeF() * BadgeFontScale);
    const QFontMetrics metrics(font);

    const QString label = QCoreApplication::translate("ChatDelegate", "GIF");
    const QRect badge(mediaRect.left() + AccessoryInset, mediaRect.top() + AccessoryInset, metrics.horizontalAdvance(label) + 2 * BadgePaddingX, metrics.height() + 2 * BadgePaddingY);
    if (!mediaRect.contains(badge))
        return;

    painter->save();
    drawBackdrop(painter, badge);
    painter->setFont(font);
    painter->setPen(Qt::white);
    painter->drawText(badge, Qt::AlignCenter, label);
    painter->restore();
}

void drawClipFrame(QPainter *painter, const QRect &rect, const QImage &frame, ClipFit fit)
{
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (fit == ClipFit::Crop) {
        painter->drawImage(rect, frame, ChatLayout::centredCrop(frame.size(), rect.size()));
    } else {
        const QSize fitted = frame.size().scaled(rect.size(), Qt::KeepAspectRatio);
        const bool fillsRect = rect.width() - fitted.width() <= SeamlessFitTolerancePx && rect.height() - fitted.height() <= SeamlessFitTolerancePx;
        painter->drawImage(fillsRect ? rect : QRect(rect.topLeft(), fitted), frame);
    }

    painter->restore();
}

QRect starRect(const QRect &mediaRect)
{
    const QRect star(mediaRect.right() + 1 - AccessoryInset - StarButtonPx, mediaRect.top() + AccessoryInset, StarButtonPx, StarButtonPx);
    return mediaRect.contains(star) ? star : QRect();
}

void drawStar(QPainter *painter, const QRect &mediaRect, GifStar star)
{
    const QRect button = starRect(mediaRect);
    if (star == GifStar::Hidden || button.isEmpty())
        return;

    using namespace Core::Theme;
    const qreal dpr = painter->device()->devicePixelRatioF();
    const QPixmap icon = star == GifStar::Filled ? Icons::pixmap(Icons::Name::StarFilled, StarIconPx, FavoritedStarColor, dpr)
                                                 : Icons::pixmap(Icons::Name::Star, StarIconPx, QColor(Qt::white), dpr);

    painter->save();
    drawBackdrop(painter, button);
    const int iconOffset = (StarButtonPx - StarIconPx) / 2;
    painter->drawPixmap(button.topLeft() + QPoint(iconOffset, iconOffset), icon);
    painter->restore();
}

} // namespace GifPainting
} // namespace UI
} // namespace Acheron
