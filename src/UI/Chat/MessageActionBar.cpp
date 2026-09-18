#include "UI/Chat/MessageActionBar.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QToolButton>

#include "Core/Theme/Icons.hpp"

namespace Acheron {
namespace UI {

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

QToolButton *MessageActionBar::addButton(const QString &iconName, const QString &toolTip, Core::Theme::Token color)
{
    constexpr int IconSize = 18;
    constexpr int ButtonSize = 24;

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
    copyIdButton->setVisible(shiftHeld);
    copyLinkButton->setVisible(shiftHeld);
    deleteButton->setVisible(showDelete);
    moreButton->setVisible(!showDelete);
}

} // namespace UI
} // namespace Acheron
