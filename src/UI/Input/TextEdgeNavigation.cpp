#include "TextEdgeNavigation.hpp"

#include <QKeyEvent>
#include <QTextCursor>
#include <QTextEdit>

namespace Acheron {
namespace UI {

static bool hasLineToMoveTo(QTextCursor cursor, bool upward)
{
    return cursor.movePosition(upward ? QTextCursor::Up : QTextCursor::Down);
}

bool moveCursorToTextEdgeFromOuterLine(QTextEdit *edit, const QKeyEvent *e)
{
    if (e->key() != Qt::Key_Up && e->key() != Qt::Key_Down)
        return false;
    const Qt::KeyboardModifiers modifiers = e->modifiers() & ~Qt::KeypadModifier;
    if (modifiers != Qt::NoModifier && modifiers != Qt::ShiftModifier)
        return false;

    const bool upward = e->key() == Qt::Key_Up;
    QTextCursor cursor = edit->textCursor();
    if (hasLineToMoveTo(cursor, upward))
        return false;

    const QTextCursor::MoveMode mode = modifiers == Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;
    cursor.movePosition(upward ? QTextCursor::Start : QTextCursor::End, mode);
    edit->setTextCursor(cursor);
    return true;
}

} // namespace UI
} // namespace Acheron
