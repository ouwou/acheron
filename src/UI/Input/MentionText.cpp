#include "MentionText.hpp"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

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

} // namespace

void insertMention(QTextCursor &cursor, const QString &displayText, const QString &mentionText, QTextCharFormat format)
{
    format.setAnchor(true);
    format.setAnchorHref(mentionText);
    format.setProperty(DisplayTextProperty, displayText);
    cursor.insertText(displayText, format);
    cursor.insertText(QStringLiteral(" "), QTextCharFormat());
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
