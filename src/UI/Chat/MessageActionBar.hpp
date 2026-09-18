#pragma once

#include <QFrame>

#include "Core/Theme/Tokens.hpp"

class QToolButton;

namespace Acheron {
namespace UI {

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

    explicit MessageActionBar(QWidget *parent = nullptr);

    static QString label(Action action);

    void setShiftHeld(bool held);
    void setCanDelete(bool canDelete);
    void setMoreButtonDown(bool down);

signals:
    void triggered(Acheron::UI::MessageActionBar::Action action);
    void moreRequested(const QPoint &buttonTopLeft);
    void shiftHeldChanged(bool held);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QToolButton *addButton(const QString &iconName, const QString &toolTip, Core::Theme::Token color);
    QToolButton *addActionButton(Action action, const QString &iconName, Core::Theme::Token color);
    void updateButtons();

    QToolButton *copyIdButton = nullptr;
    QToolButton *copyLinkButton = nullptr;
    QToolButton *deleteButton = nullptr;
    QToolButton *moreButton = nullptr;

    bool shiftHeld = false;
    bool canDelete = false;
};

} // namespace UI
} // namespace Acheron
