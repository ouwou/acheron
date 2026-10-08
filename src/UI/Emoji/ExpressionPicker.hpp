#pragma once

#include <QFrame>

#include "Core/Emoji/EmojiManager.hpp"
#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/Gifs/FavoriteGifs.hpp"
#include "Core/Snowflake.hpp"

class QLineEdit;
class QStackedWidget;
class QToolButton;

namespace Acheron {

namespace Core {
class AnimatedImageCache;
class ImageManager;
} // namespace Core

namespace UI {

class EmojiPicker;
class FavoriteGifPicker;
class GifPlayback;

class ExpressionPicker : public QFrame
{
    Q_OBJECT
public:
    enum class Tab {
        Gifs,
        Emoji,
    };

    ExpressionPicker(Core::ImageManager *imageManager, Core::AnimatedImageCache *animatedCache, QWidget *parent = nullptr);

    void setAnimationEnabled(bool enabled);
    [[nodiscard]] GifPlayback *gifPlayback() const;

    void openForReaction(Core::EmojiManager *emojiManager, Core::Snowflake accountId, Core::Snowflake channelId, const QRect &globalAnchor);
    void openForChat(Core::EmojiManager *emojiManager, Core::FavoriteGifs *favoriteGifs, Core::Snowflake accountId, Core::Snowflake channelId, Tab tab, const QRect &globalAnchor);

signals:
    void emojiPicked(const Acheron::Core::PickerEmoji &emoji, bool pickerStaysOpen);
    void gifPicked(const QString &url);
    void chatTabShown(Acheron::UI::ExpressionPicker::Tab tab);
    void closed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    enum class Placement {
        BesideAnchor,
        AboveAnchor,
    };

    void showTab(Tab tab);
    void placeAt(const QRect &globalAnchor, Placement placement);
    void onTabShortcut(Tab tab);
    [[nodiscard]] QLineEdit *currentSearchField() const;

    QWidget *tabRow;
    QToolButton *gifTab;
    QToolButton *emojiTab;
    QStackedWidget *pages;
    FavoriteGifPicker *gifPage;
    EmojiPicker *emojiPage;
};

} // namespace UI
} // namespace Acheron
