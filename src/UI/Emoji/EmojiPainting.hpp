#pragma once

#include <QPixmap>
#include <QRect>
#include <QUrl>

#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/Snowflake.hpp"

class QPainter;

namespace Acheron {
namespace UI {

class PaintedImages;

[[nodiscard]] QUrl emojiStillUrl(Core::Snowflake emojiId, qreal devicePixelRatio);
[[nodiscard]] QUrl emojiAnimationUrl(Core::Snowflake emojiId, qreal devicePixelRatio);

void drawCentered(QPainter &painter, const QRect &box, const QPixmap &pixmap);

bool drawEmojiStill(QPainter &painter, const QRect &squareBox, const Core::PickerEmoji &emoji, PaintedImages &images, const QRect &repaintRect);

} // namespace UI
} // namespace Acheron
