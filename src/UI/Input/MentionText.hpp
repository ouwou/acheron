#pragma once

#include <QString>
#include <QTextCharFormat>

#include <functional>

#include "Core/Snowflake.hpp"

class QTextCursor;
class QTextDocument;

namespace Acheron {
namespace UI {

enum class MentionKind {
    User,
    Role,
    Channel,
};
using MentionNameResolver = std::function<QString(MentionKind kind, Core::Snowflake id)>;

void insertMention(QTextCursor &cursor, const QString &displayText, const QString &mentionText, QTextCharFormat format);
void insertWithMentions(QTextCursor &cursor, const QString &text, const MentionNameResolver &resolve, const QTextCharFormat &format);
[[nodiscard]] QString toMessageText(const QTextDocument *document);
void restyleMentions(QTextDocument *document, const QTextCharFormat &format);

} // namespace UI
} // namespace Acheron
