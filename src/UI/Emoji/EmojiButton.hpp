#pragma once

#include <QAbstractButton>

#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/Snowflake.hpp"

namespace Acheron {

namespace Core {
class ImageManager;
}

namespace UI {

class PaintedImages;

class EmojiButton : public QAbstractButton
{
    Q_OBJECT
public:
    EmojiButton(int buttonPx, int emojiPx, QWidget *parent = nullptr);

    void setImageSource(Core::ImageManager *imageManager, Core::Snowflake accountId);
    void setEmoji(const Core::PickerEmoji &emoji);
    [[nodiscard]] const Core::PickerEmoji &emoji() const { return shownEmoji; }
    void setReacted(bool reacted);
    [[nodiscard]] bool isReacted() const { return reacted; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    PaintedImages *images;
    Core::PickerEmoji shownEmoji;
    int emojiPx;
    bool reacted = false;
};

} // namespace UI
} // namespace Acheron
