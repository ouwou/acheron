#include "Core/Emoji/UnicodeEmojiIndex.hpp"

#include "Core/Logging.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPair>

#include <iterator>

namespace Acheron {
namespace Core {

namespace {

const char *const SKIN_TONE_CODEPOINTS[] = { "1f3fb", "1f3fc", "1f3fd", "1f3fe", "1f3ff" };

// N or 0 if not a skin tone
int toneNumber(const QString &codepoint)
{
    for (int i = 0; i < int(std::size(SKIN_TONE_CODEPOINTS)); i++) {
        if (codepoint == SKIN_TONE_CODEPOINTS[i])
            return i + 1;
    }
    return 0;
}

QStringList toStringList(const QJsonValue &value)
{
    const QJsonArray array = value.toArray();
    QStringList list;
    list.reserve(array.size());
    for (const QJsonValue &item : array)
        list.append(item.toString());
    return list;
}

} // namespace

const QRegularExpression &customEmojiTagRe()
{
    static const QRegularExpression re(QStringLiteral("<a?:([a-zA-Z0-9_]{1,32}):(\\d+)>"));
    return re;
}

// `:100:` but not`<:100:123>` (and skin tones)
static QList<QRegularExpressionMatch> shortcodesOutsideTags(const QString &text)
{
    static const QRegularExpression shortcodeRe(
            QStringLiteral(":([^\\s:]+?(?:::skin-tone-\\d)?):"));

    QList<QPair<int, int>> tagRanges;
    auto tagIt = customEmojiTagRe().globalMatch(text);
    while (tagIt.hasNext()) {
        const QRegularExpressionMatch tag = tagIt.next();
        tagRanges.append(qMakePair(int(tag.capturedStart()), int(tag.capturedEnd())));
    }

    QList<QRegularExpressionMatch> shortcodes;
    auto it = shortcodeRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        bool insideTag = false;
        for (const auto &range : tagRanges) {
            if (match.capturedStart() < range.second && match.capturedEnd() > range.first) {
                insideTag = true;
                break;
            }
        }
        if (!insideTag)
            shortcodes.append(match);
    }

    return shortcodes;
}

UnicodeEmojiIndex &UnicodeEmojiIndex::instance()
{
    static UnicodeEmojiIndex index;
    return index;
}

UnicodeEmojiIndex::UnicodeEmojiIndex()
{
    QFile file(QStringLiteral(":/resources/emoji_data.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(LogCore) << "UnicodeEmojiIndex: cannot open" << file.fileName();
        return;
    }

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (!doc.isObject()) {
        qCWarning(LogCore) << "UnicodeEmojiIndex: parse failed:" << error.errorString();
        return;
    }

    const QJsonArray entries = doc.object().value(QStringLiteral("emojis")).toArray();
    emojis.reserve(entries.size());
    for (const QJsonValue &value : entries) {
        const QJsonObject obj = value.toObject();
        UnicodeEmoji emoji;
        emoji.names = toStringList(obj.value(QStringLiteral("names")));
        emoji.keywords = toStringList(obj.value(QStringLiteral("keywords")));
        emoji.surrogates = obj.value(QStringLiteral("surrogates")).toString();
        emoji.hasDiversity = obj.value(QStringLiteral("hasDiversity")).toBool();
        emoji.isDiversityChild = obj.value(QStringLiteral("hasDiversityParent")).toBool() || obj.value(QStringLiteral("hasMultiDiversityParent")).toBool();
        emojis.append(emoji);
    }

    for (int i = 0; i < emojis.size(); i++) {
        const UnicodeEmoji &emoji = emojis.at(i);
        for (const QString &name : emoji.names)
            nameToIndex.insert(name, i);
        surrogateToIndex.insert(emoji.surrogates, i);
        if (!emoji.isDiversityChild)
            topLevelEmojis.append(&emoji);
        if (!emoji.hasDiversity)
            continue;

        const QJsonArray children = entries.at(i).toObject().value(QStringLiteral("diversityChildren")).toArray();
        for (const QJsonValue &child : children) {
            const int childIndex = child.toInt(-1);
            if (childIndex < 0 || childIndex >= emojis.size())
                continue;
            const QJsonArray tones = entries.at(childIndex).toObject().value(QStringLiteral("diversity")).toArray();
            const int tone = toneNumber(tones.at(0).toString());
            if (tone == 0)
                continue;
            const QString suffix = QStringLiteral("::skin-tone-") + QString::number(tone);
            for (const QString &name : emoji.names)
                nameToIndex.insert(name + suffix, childIndex);
        }
    }

    qCDebug(LogCore) << "UnicodeEmojiIndex: loaded" << emojis.size() << "emojis";
}

const UnicodeEmoji *UnicodeEmojiIndex::byName(const QString &name) const
{
    const auto it = nameToIndex.constFind(name);
    if (it == nameToIndex.constEnd())
        return nullptr;
    return &emojis.at(it.value());
}

const UnicodeEmoji *UnicodeEmojiIndex::bySurrogate(const QString &surrogates) const
{
    const auto it = surrogateToIndex.constFind(surrogates);
    if (it == surrogateToIndex.constEnd())
        return nullptr;
    return &emojis.at(it.value());
}

const UnicodeEmoji *UnicodeEmojiIndex::convertSurrogateToBase(const QString &surrogates) const
{
    QString base;
    base.reserve(surrogates.size());
    for (int i = 0; i < surrogates.size(); i++) {
        const ushort unit = surrogates.at(i).unicode();
        if (unit == 0xD83C && i + 1 < surrogates.size()) {
            const ushort low = surrogates.at(i + 1).unicode();
            if (low >= 0xDFFB && low <= 0xDFFF) {
                i++;
                continue;
            }
        }
        base.append(surrogates.at(i));
    }
    return bySurrogate(base);
}

QString UnicodeEmojiIndex::translateNamesToSurrogates(const QString &text) const
{
    QString result;
    int copied = 0;

    for (const QRegularExpressionMatch &match : shortcodesOutsideTags(text)) {
        const UnicodeEmoji *emoji = byName(match.captured(1));
        if (!emoji)
            continue;

        result += text.mid(copied, match.capturedStart() - copied);
        result += emoji->surrogates;
        copied = int(match.capturedEnd());
    }

    if (copied == 0)
        return text;

    result += text.mid(copied);
    return result;
}

} // namespace Core
} // namespace Acheron
