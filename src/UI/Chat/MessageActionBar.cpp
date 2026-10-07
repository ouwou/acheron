#include "UI/Chat/MessageActionBar.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QToolButton>

#include "Core/Theme/Icons.hpp"
#include "UI/Emoji/EmojiButton.hpp"

namespace Acheron {
namespace UI {

namespace {

constexpr int ButtonSize = 24;
constexpr int IconSize = 18;

} // namespace

MessageActionBar::MessageActionBar(QWidget *parent) : QFrame(parent)
{
    using namespace Core::Theme;

    setObjectName("MessageActionBar");
    setCursor(Qt::ArrowCursor);
    setContextMenuPolicy(Qt::PreventContextMenu);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    copyIdButton = addActionButton(Action::CopyId, Icons::Name::IdCard, Token::ButtonText);
    copyLinkButton = addActionButton(Action::CopyLink, Icons::Name::Link, Token::ButtonText);
    addQuickReactionButtons();
    reactionPickerButton = addButton(Icons::Name::SmilePlus, tr("Add Reaction"), Token::ButtonText);
    connect(reactionPickerButton, &QToolButton::clicked, this, &MessageActionBar::reactionPickerRequested);
    addActionButton(Action::Reply, Icons::Name::Reply, Token::ButtonText);
    deleteButton = addActionButton(Action::Delete, Icons::Name::Trash, Token::ChatError);
    moreButton = addButton(Icons::Name::Ellipsis, tr("More"), Token::ButtonText);
    connect(moreButton, &QToolButton::clicked, this, [this]() {
        emit moreRequested(moreButton->mapToGlobal(QPoint(0, 0)));
    });

    updateButtons();
    setVisible(false);

    qApp->installEventFilter(this);
}

QString MessageActionBar::label(Action action)
{
    switch (action) {
    case Action::CopyId:
        return tr("Copy Message ID");
    case Action::CopyLink:
        return tr("Copy Message Link");
    case Action::Reply:
        return tr("Reply");
    case Action::Delete:
        return tr("Delete Message");
    }
    return {};
}

void MessageActionBar::setShiftHeld(bool held)
{
    held = held && !reactionPickerOpen;
    if (shiftHeld == held)
        return;
    shiftHeld = held;
    updateButtons();
    emit shiftHeldChanged(held);
}

void MessageActionBar::setCanDelete(bool canDelete)
{
    if (this->canDelete == canDelete)
        return;
    this->canDelete = canDelete;
    updateButtons();
}

void MessageActionBar::setMoreButtonDown(bool down)
{
    moreButton->setDown(down);
}

void MessageActionBar::setImageSource(Core::ImageManager *imageManager, Core::Snowflake accountId)
{
    for (EmojiButton *button : quickReactionButtons)
        button->setImageSource(imageManager, accountId);
}

void MessageActionBar::setReactions(bool canReact, const QList<QuickReaction> &quickReactions)
{
    this->canReact = canReact;
    quickReactionCount = qMin(int(quickReactions.size()), QuickReactionCount);

    for (int i = 0; i < quickReactionCount; i++) {
        const QuickReaction &quick = quickReactions.at(i);
        EmojiButton *button = quickReactionButtons.at(i);
        button->setEmoji(quick.emoji);
        button->setReacted(quick.reacted);
        button->setToolTip(":" + quick.emoji.name + ":\n" + (quick.reacted ? tr("Click to remove") : tr("Click to react")));
    }
    updateButtons();
}

void MessageActionBar::setReactionPickerOpen(bool open)
{
    reactionPickerOpen = open;
    reactionPickerButton->setDown(open);
    if (open)
        setShiftHeld(false);
}

QRect MessageActionBar::reactionPickerButtonGlobalRect() const
{
    return QRect(reactionPickerButton->mapToGlobal(QPoint(0, 0)), reactionPickerButton->size());
}

bool MessageActionBar::eventFilter(QObject *obj, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Shift)
            setShiftHeld(event->type() == QEvent::KeyPress);
        else
            setShiftHeld(keyEvent->modifiers().testFlag(Qt::ShiftModifier));
        break;
    }
    case QEvent::WindowDeactivate:
        if (obj == window())
            setShiftHeld(false);
        break;
    default:
        break;
    }
    return QFrame::eventFilter(obj, event);
}

void MessageActionBar::addQuickReactionButtons()
{
    constexpr int SeparatorHeight = 16;

    for (int i = 0; i < QuickReactionCount; i++) {
        auto *button = new EmojiButton(ButtonSize, IconSize, this);
        connect(button, &EmojiButton::clicked, this, [this, button]() {
            emit quickReactionTriggered(button->emoji(), button->isReacted());
        });
        layout()->addWidget(button);
        quickReactionButtons.append(button);
    }

    quickReactionSeparator = new QFrame(this);
    quickReactionSeparator->setFrameShape(QFrame::VLine);
    quickReactionSeparator->setFixedHeight(SeparatorHeight);
    layout()->addWidget(quickReactionSeparator);
}

QToolButton *MessageActionBar::addButton(const QString &iconName, const QString &toolTip, Core::Theme::Token color)
{
    auto *button = new QToolButton(this);
    button->setIcon(Core::Theme::Icons::icon(iconName, color));
    button->setIconSize(QSize(IconSize, IconSize));
    button->setFixedSize(ButtonSize, ButtonSize);
    button->setToolTip(toolTip);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setCursor(Qt::PointingHandCursor);
    layout()->addWidget(button);
    return button;
}

QToolButton *MessageActionBar::addActionButton(Action action, const QString &iconName, Core::Theme::Token color)
{
    QToolButton *button = addButton(iconName, label(action), color);
    connect(button, &QToolButton::clicked, this, [this, action]() { emit triggered(action); });
    return button;
}

void MessageActionBar::updateButtons()
{
    const bool showDelete = shiftHeld && canDelete;
    const bool showQuickReactions = canReact && !shiftHeld;
    copyIdButton->setVisible(shiftHeld);
    copyLinkButton->setVisible(shiftHeld);
    for (int i = 0; i < quickReactionButtons.size(); i++)
        quickReactionButtons.at(i)->setVisible(showQuickReactions && i < quickReactionCount);
    quickReactionSeparator->setVisible(showQuickReactions && quickReactionCount > 0);
    reactionPickerButton->setVisible(canReact);
    deleteButton->setVisible(showDelete);
    moreButton->setVisible(!showDelete);
}

} // namespace UI
} // namespace Acheron
