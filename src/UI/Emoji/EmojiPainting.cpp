#include "UI/Emoji/EmojiPainting.hpp"

#include <QPaintDevice>
#include <QPainter>

#include "Discord/CdnUrls.hpp"
#include "UI/Emoji/EmojiGlyphs.hpp"
#include "UI/Emoji/PaintedImages.hpp"

namespace Acheron {
namespace UI {

namespace {

int emojiAssetPx(qreal devicePixelRatio)
{
    constexpr int AssetPx = 48;
    constexpr int HighDpiAssetPx = 96;
    return devicePixelRatio > 1.0 ? HighDpiAssetPx : AssetPx;
}

} // namespace

QUrl emojiStillUrl(Core::Snowflake emojiId, qreal devicePixelRatio)
{
    return Discord::Cdn::emoji(emojiId, emojiAssetPx(devicePixelRatio), false);
}

QUrl emojiAnimationUrl(Core::Snowflake emojiId, qreal devicePixelRatio)
{
    return Discord::Cdn::emoji(emojiId, emojiAssetPx(devicePixelRatio), true);
}

void drawCentered(QPainter &painter, const QRect &box, const QPixmap &pixmap)
{
    QSize logical = pixmap.size() / pixmap.devicePixelRatio();
    if (logical.width() > box.width() || logical.height() > box.height())
        logical.scale(box.size(), Qt::KeepAspectRatio);

    const QRect target(box.left() + (box.width() - logical.width()) / 2, box.top() + (box.height() - logical.height()) / 2, logical.width(), logical.height());
    painter.drawPixmap(target, pixmap);
}

bool drawEmojiStill(QPainter &painter, const QRect &squareBox, const Core::PickerEmoji &emoji, PaintedImages &images, const QRect &repaintRect)
{
    const qreal devicePixelRatio = painter.device()->devicePixelRatioF();
    const int px = squareBox.width();

    if (!emoji.isCustom()) {
        painter.drawPixmap(squareBox.topLeft(), EmojiGlyphs::pixmap(emoji.surrogates, px, devicePixelRatio));
        return true;
    }

    const QPixmap still = images.pixmap(emojiStillUrl(emoji.customId, devicePixelRatio), px, repaintRect);
    if (still.isNull())
        return false;
    drawCentered(painter, squareBox, still);
    return true;
}

} // namespace UI
} // namespace Acheron
