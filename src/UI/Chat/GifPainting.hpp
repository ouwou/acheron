#pragma once

#include <QFont>
#include <QImage>
#include <QRect>

#include "UI/Chat/GifPlayback.hpp"

class QPainter;

namespace Acheron {
namespace UI {
namespace GifPainting {

void drawBadge(QPainter *painter, const QRect &mediaRect, const QFont &baseFont);
void drawClipFrame(QPainter *painter, const QRect &rect, const QImage &frame, ClipFit fit);

[[nodiscard]] QRect starRect(const QRect &mediaRect);
void drawStar(QPainter *painter, const QRect &mediaRect, GifStar star);

} // namespace GifPainting
} // namespace UI
} // namespace Acheron
