#pragma once

#include <QFrame>
#include <QList>

#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Theme/Tokens.hpp"

class QToolButton;

namespace Acheron {

namespace Core {
class ImageManager;
}

namespace UI {

class EmojiButton;

class MessageActionBar : public QFrame
{
    Q_OBJECT
public:
    enum class Action {
        CopyId,
        CopyLink,
        Reply,
        Delete,
    };
    Q_ENUM(Action)

    static constexpr int QuickReactionCount = 3;

    struct QuickReaction
    {
        Core::PickerEmoji emoji;
        bool reacted = false;
    };

    explicit MessageActionBar(QWidget *parent = nullptr);

    static QString label(Action action);

    void setShiftHeld(bool held);
    void setCanDelete(bool canDelete);
    void setMoreButtonDown(bool down);

    void setImageSource(Core::ImageManager *imageManager, Core::Snowflake accountId);
    void setReactions(bool canReact, const QList<QuickReaction> &quickReactions);
    void setReactionPickerOpen(bool open);
    [[nodiscard]] QRect reactionPickerButtonGlobalRect() const;

signals:
    void triggered(Acheron::UI::MessageActionBar::Action action);
    void moreRequested(const QPoint &buttonTopLeft);
    void shiftHeldChanged(bool held);
    void quickReactionTriggered(const Acheron::Core::PickerEmoji &emoji, bool reacted);
    void reactionPickerRequested();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QToolButton *addButton(const QString &iconName, const QString &toolTip, Core::Theme::Token color);
    QToolButton *addActionButton(Action action, const QString &iconName, Core::Theme::Token color);
    void addQuickReactionButtons();
    void updateButtons();

    QToolButton *copyIdButton = nullptr;
    QToolButton *copyLinkButton = nullptr;
    QList<EmojiButton *> quickReactionButtons;
    QFrame *quickReactionSeparator = nullptr;
    QToolButton *reactionPickerButton = nullptr;
    QToolButton *deleteButton = nullptr;
    QToolButton *moreButton = nullptr;

    bool shiftHeld = false;
    bool canDelete = false;
    bool canReact = false;
    bool reactionPickerOpen = false;
    int quickReactionCount = 0;
};

} // namespace UI
} // namespace Acheron
