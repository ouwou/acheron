#pragma once

#include <QHash>
#include <QList>
#include <QPointer>
#include <QWidget>

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

class EmojiPicker : public QWidget
{
    Q_OBJECT
public:
    EmojiPicker(Core::ImageManager *imageManager, Core::AnimatedImageCache *animatedCache, QWidget *parent = nullptr);

    void setAnimationEnabled(bool enabled);
    void prepareFor(Core::EmojiManager *emojiManager, Core::Snowflake accountId, Core::Snowflake channelId, Core::EmojiIntention intention);
    [[nodiscard]] QLineEdit *searchField() const { return search; }

signals:
    void emojiPicked(const Acheron::Core::PickerEmoji &emoji, bool pickerStaysOpen);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
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
