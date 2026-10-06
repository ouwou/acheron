#pragma once

class QKeyEvent;
class QTextEdit;

namespace Acheron {
namespace UI {

[[nodiscard]] bool moveCursorToTextEdgeFromOuterLine(QTextEdit *edit, const QKeyEvent *e);

} // namespace UI
} // namespace Acheron
