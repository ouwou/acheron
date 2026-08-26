#pragma once

#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace Acheron {
namespace Core {

// `<:name:id>` / `<a:name:id>`.
const QRegularExpression &customEmojiTagRe();

struct UnicodeEmoji
{
    // aliases: primary is first and is used as frecency tracker key
    QStringList names;
    QStringList keywords;
    QString surrogates;
    bool hasDiversity = false;
    bool isDiversityChild = false;
};

// index extracted from client
class UnicodeEmojiIndex
{
public:
    static UnicodeEmojiIndex &instance();

    // by any alias
    [[nodiscard]] const UnicodeEmoji *byName(const QString &name) const;
    [[nodiscard]] const UnicodeEmoji *bySurrogate(const QString &surrogates) const;
    // toned to untoned
    [[nodiscard]] const UnicodeEmoji *convertSurrogateToBase(const QString &surrogates) const;

    [[nodiscard]] static QString primaryName(const UnicodeEmoji &emoji) { return emoji.names.value(0); }

    // non-diversity
    [[nodiscard]] const QList<const UnicodeEmoji *> &topLevel() const { return topLevelEmojis; }

    // `:name:` / `:name::skin-tone-N:` to surrogates
    [[nodiscard]] QString translateNamesToSurrogates(const QString &text) const;

private:
    UnicodeEmojiIndex();

    // fixed
    QList<UnicodeEmoji> emojis;
    QHash<QString, int> nameToIndex;
    QHash<QString, int> surrogateToIndex;
    QList<const UnicodeEmoji *> topLevelEmojis; // -> emojis
};

} // namespace Core
} // namespace Acheron
