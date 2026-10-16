#pragma once

#include <QString>
#include <QTextCharFormat>

class QTextCursor;
class QTextDocument;

namespace Acheron {
namespace UI {

void insertMention(QTextCursor &cursor, const QString &displayText, const QString &mentionText, QTextCharFormat format);
[[nodiscard]] QString toMessageText(const QTextDocument *document);

} // namespace UI
} // namespace Acheron
