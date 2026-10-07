#pragma once

#include <QFrame>
#include <QHash>
#include <QList>
#include <QPointer>

#include "Core/Emoji/EmojiManager.hpp"
#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/Snowflake.hpp"

class QLineEdit;

namespace Acheron {

namespace Core {
class AnimatedImageCache;
class ImageManager;
} // namespace Core

namespace UI {

class EmojiGridView;
class EmojiInspector;
class EmojiSectionRail;

class EmojiPicker : public QFrame
{
    Q_OBJECT
public:
    EmojiPicker(Core::ImageManager *imageManager, Core::AnimatedImageCache *animatedCache, QWidget *parent = nullptr);

    enum class Placement {
        BesideAnchor,
        AboveAnchor,
    };

    void setAnimationEnabled(bool enabled);
    void openFor(Core::EmojiManager *emojiManager, Core::Snowflake accountId, Core::Snowflake channelId, Core::EmojiIntention intention, const QRect &globalAnchor, Placement placement);

signals:
    void emojiPicked(const Acheron::Core::PickerEmoji &emoji, bool pickerStaysOpen);
    void closed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void placeAt(const QRect &globalAnchor, Placement placement);
    [[nodiscard]] QString searchPrompt() const;
    void showSearchResults(const QString &text);
    void showBrowseSections();
    void pick(const Core::PickerEmoji &emoji, bool keepOpen);
    void onActiveEmojiChanged();
    void onSectionClicked(int section);
    void saveCollapsedSections();
    bool handleSearchKey(const QKeyEvent *key);

    Core::ImageManager *imageManager;
    Core::AnimatedImageCache *animatedCache;

    QLineEdit *search;
    EmojiSectionRail *rail;
    EmojiGridView *grid;
    EmojiInspector *inspector;

    QPointer<Core::EmojiManager> emojis;
    Core::Snowflake channelId;
    Core::EmojiIntention intention = Core::EmojiIntention::Reaction;
    QList<Core::PickerSection> browseSections;
    QHash<Core::Snowflake, QString> guildNames;
    bool searching = false;
};

} // namespace UI
} // namespace Acheron
