#include "MentionText.hpp"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <utility>

namespace Acheron {
namespace UI {

namespace {

constexpr int DisplayTextProperty = QTextFormat::UserProperty;

QString plainFragmentText(QString text)
{
    text.replace(QChar::Nbsp, QLatin1Char(' '));
    text.replace(QChar::LineSeparator, QLatin1Char('\n'));
    return text;
}

void insertMentionText(QTextCursor &cursor, const QString &displayText, const QString &mentionText, QTextCharFormat format)
{
    format.setAnchor(true);
    format.setAnchorHref(mentionText);
    format.setProperty(DisplayTextProperty, displayText);
    cursor.insertText(displayText, format);
}

} // namespace

void insertMention(QTextCursor &cursor, const QString &displayText, const QString &mentionText, QTextCharFormat format)
{
    insertMentionText(cursor, displayText, mentionText, std::move(format));
    cursor.insertText(QStringLiteral(" "), QTextCharFormat());
}

void insertWithMentions(QTextCursor &cursor, const QString &text, const MentionNameResolver &resolve, const QTextCharFormat &format)
{
    static const QRegularExpression mentionPattern(QStringLiteral(R"(<(@!?|@&|#)(\d+)>)"));
    qsizetype last = 0;
    for (auto it = mentionPattern.globalMatch(text); resolve && it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const QString sigil = match.captured(1);
        const MentionKind kind = sigil == QLatin1String("@&") ? MentionKind::Role : sigil == QLatin1String("#") ? MentionKind::Channel
                                                                                                                : MentionKind::User;
        const QString displayText = resolve(kind, Core::Snowflake(match.captured(2).toULongLong()));
        if (displayText.isEmpty())
            continue;
        cursor.insertText(text.mid(last, match.capturedStart() - last), QTextCharFormat());
        insertMentionText(cursor, displayText, match.captured(), format);
        last = match.capturedEnd();
    }
    cursor.insertText(text.mid(last), QTextCharFormat());
}

void restyleMentions(QTextDocument *document, const QTextCharFormat &format)
{
    // collected first: changing a format can merge the fragments being iterated
    QList<std::pair<int, int>> mentions;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.charFormat().isAnchor() && fragment.charFormat().hasProperty(DisplayTextProperty))
                mentions.append({ fragment.position(), fragment.length() });
        }
    }
    QTextCursor cursor(document);
    for (const auto &[position, length] : mentions) {
        cursor.setPosition(position);
        cursor.setPosition(position + length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
}

QString toMessageText(const QTextDocument *document)
{
    QString text;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        if (block != document->begin())
            text += QLatin1Char('\n');
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat format = fragment.charFormat();
            if (format.isAnchor() && fragment.text() == format.stringProperty(DisplayTextProperty))
                text += format.anchorHref();
            else
                text += plainFragmentText(fragment.text());
        }
    }
    return text;
}

} // namespace UI
} // namespace Acheron
