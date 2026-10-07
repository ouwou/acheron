#include "UI/Emoji/EmojiButton.hpp"

#include <QPainter>

#include "Core/Theme/Manager.hpp"
#include "UI/Emoji/EmojiPainting.hpp"
#include "UI/Emoji/PaintedImages.hpp"

namespace Acheron {
namespace UI {

EmojiButton::EmojiButton(int buttonPx, int emojiPx, QWidget *parent)
    : QAbstractButton(parent), images(new PaintedImages(this)), emojiPx(emojiPx)
{
    setFixedSize(buttonPx, buttonPx);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
}

void EmojiButton::setImageSource(Core::ImageManager *imageManager, Core::Snowflake accountId)
{
    images->setSource(imageManager, accountId);
}

void EmojiButton::setEmoji(const Core::PickerEmoji &emoji)
{
    shownEmoji = emoji;
    update();
}

void EmojiButton::setReacted(bool reacted)
{
    if (this->reacted == reacted)
        return;
    this->reacted = reacted;
    update();
}

void EmojiButton::paintEvent(QPaintEvent *)
{
    constexpr int Radius = 6;
    constexpr int HoverAlpha = 24;
    constexpr int PressedAlpha = 40;
    constexpr int ReactedAlpha = 45;
    using namespace Core::Theme;
    const Manager &theme = Manager::instance();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF background = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (reacted) {
        QColor fill = theme.color(Token::Highlight);
        fill.setAlpha(ReactedAlpha);
        painter.setPen(theme.color(Token::Highlight));
        painter.setBrush(fill);
        painter.drawRoundedRect(background, Radius, Radius);
    }
    if (underMouse() || isDown()) {
        QColor tint = theme.color(Token::PrimaryText);
        tint.setAlpha(isDown() ? PressedAlpha : HoverAlpha);
        painter.setPen(Qt::NoPen);
        painter.setBrush(tint);
        painter.drawRoundedRect(background, Radius, Radius);
    }

    const QRect emojiRect((width() - emojiPx) / 2, (height() - emojiPx) / 2, emojiPx, emojiPx);
    drawEmojiStill(painter, emojiRect, shownEmoji, *images, rect());
}

} // namespace UI
} // namespace Acheron
