#include "MentionSearch.hpp"

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVector>

#include <algorithm>

namespace Acheron {
namespace Core {
namespace MentionSearch {

namespace {

const QHash<char32_t, QString> &confusables()
{
    static const QHash<char32_t, QString> table = [] {
        QHash<char32_t, QString> result;
        QFile file(QStringLiteral(":/resources/confusables.json"));
        if (!file.open(QIODevice::ReadOnly))
            return result;
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const auto key = it.key().toUcs4();
            if (key.size() == 1)
                result.insert(key.first(), it.value().toString());
        }
        return result;
    }();
    return table;
}

void appendCodePoint(QString &text, uint codePoint)
{
    if (QChar::requiresSurrogates(codePoint)) {
        text += QChar(QChar::highSurrogate(codePoint));
        text += QChar(QChar::lowSurrogate(codePoint));
    } else {
        text += QChar(char16_t(codePoint));
    }
}

QList<int> sortedByScore(const QVector<double> &scores, const QList<int> &indices)
{
    QList<int> sorted = indices;
    std::stable_sort(sorted.begin(), sorted.end(), [&scores](int a, int b) { return scores[a] > scores[b]; });
    return sorted;
}

struct ChannelToken
{
    QString lower;
    QStringList spellings; // the whole-query token also matches a hyphen in place of its first space
    bool isFullMatch = false;
};

QList<ChannelToken> channelTokens(const QString &query)
{
    QList<ChannelToken> tokens;
    for (const QString &part : query.split(QLatin1Char(' '))) {
        const QString lower = part.toLower();
        tokens.append({ lower, { lower }, false });
    }
    if (query.contains(QLatin1Char(' '))) {
        const QString lower = query.toLower();
        QString hyphenated = lower;
        hyphenated.replace(hyphenated.indexOf(QLatin1Char(' ')), 1, QLatin1Char('-'));
        tokens.prepend({ lower, { lower, hyphenated }, true });
    }
    return tokens;
}

double tokenScore(const QString &text, const ChannelToken &token, bool fuzzy)
{
    if (std::any_of(token.spellings.begin(), token.spellings.end(), [&text](const QString &s) { return text.startsWith(s, Qt::CaseInsensitive); }))
        return text.toLower() == token.lower ? 10 : 7;
    if (std::any_of(token.spellings.begin(), token.spellings.end(), [&text](const QString &s) { return text.contains(s, Qt::CaseInsensitive); }))
        return 5;
    static const QRegularExpression wordSeparators(QStringLiteral("(?:,| )+"));
    const QStringList words = token.lower.split(wordSeparators);
    if (std::all_of(words.begin(), words.end(), [&text](const QString &word) { return text.contains(word, Qt::CaseInsensitive); }))
        return 3;
    if (fuzzy && isSubsequence(token.lower, text))
        return 1;
    return 0;
}

// a whole-query match consumes every token
double consumeBestToken(const QString &text, QList<ChannelToken> &tokens, bool fuzzy)
{
    double best = 0;
    int bestIndex = -1;
    for (int i = 0; i < tokens.size(); ++i) {
        const double score = tokenScore(text, tokens[i], fuzzy);
        if (score > best) {
            best = score;
            bestIndex = i;
        }
    }
    if (bestIndex >= 0) {
        if (tokens[bestIndex].isFullMatch)
            tokens.clear();
        else
            tokens.removeAt(bestIndex);
    }
    return best;
}

} // namespace

QString stripDiacritics(const QString &text)
{
    QString kept;
    for (const QChar c : text.normalized(QString::NormalizationForm_D))
        if (c.unicode() < 0x0300 || c.unicode() > 0x036F)
            kept += c;
    return kept.normalized(QString::NormalizationForm_C);
}

QString foldConfusables(const QString &text)
{
    const QHash<char32_t, QString> &table = confusables();
    QString folded;
    for (const uint codePoint : text.normalized(QString::NormalizationForm_D).toUcs4()) {
        const auto it = table.constFind(codePoint);
        if (it != table.constEnd())
            folded += it.value();
        else
            appendCodePoint(folded, codePoint);
    }
    return folded.normalized(QString::NormalizationForm_D).toLower();
}

bool isSubsequence(const QString &needle, const QString &haystack)
{
    int j = 0;
    for (const QChar c : needle) {
        while (j < haystack.size() && haystack[j] != c)
            ++j;
        if (j == haystack.size())
            return false;
        ++j;
    }
    return true;
}

QList<int> rankUsers(const QString &query, const QList<UserCandidate> &candidates, const std::function<bool(int index)> &accept, int limit)
{
    const QString lowerQuery = query.toLower();
    const QString foldedQuery = foldConfusables(lowerQuery);
    constexpr int maxFuzzyCandidates = 50;

    QVector<double> scores(int(candidates.size()), 0);
    QList<int> matched;
    QList<int> fuzzy;
    int fullScores = 0;
    // once limit candidates have the top score no later one can rank above them, so the rest needn't be filtered
    for (int i = 0; i < candidates.size() && fullScores < limit; ++i) {
        const UserCandidate &candidate = candidates[i];
        const QString username = candidate.username.toLower();
        const QString strippedUsername = stripDiacritics(username);
        QStringList names = { username, strippedUsername };
        QStringList strippedNames = { strippedUsername };
        QStringList foldedNames = { foldConfusables(strippedUsername) };
        for (const QString &other : { candidate.nick, candidate.globalName }) {
            if (other.isEmpty())
                continue;
            const QString lower = other.toLower();
            const QString stripped = stripDiacritics(lower);
            names << lower << stripped;
            strippedNames << stripped;
            foldedNames << foldConfusables(stripped);
        }

        auto anyStartsWith = [](const QStringList &list, const QString &prefix) {
            return std::any_of(list.begin(), list.end(), [&prefix](const QString &name) { return name.startsWith(prefix); });
        };
        const bool prefix = anyStartsWith(names, lowerQuery);
        const bool foldedPrefix = !prefix && anyStartsWith(foldedNames, foldedQuery);
        const bool fuzzyHit = !prefix && !foldedPrefix && fuzzy.size() < maxFuzzyCandidates && (std::any_of(strippedNames.begin(), strippedNames.end(), [&lowerQuery](const QString &name) { return isSubsequence(lowerQuery, name); }) || std::any_of(foldedNames.begin(), foldedNames.end(), [&foldedQuery](const QString &name) { return isSubsequence(foldedQuery, name); }));
        if (!(prefix || foldedPrefix || fuzzyHit) || (accept && !accept(i)))
            continue;
        scores[i] = prefix ? 10 : 1;
        fullScores += prefix ? 1 : 0;
        (fuzzyHit ? fuzzy : matched).append(i);
    }

    QList<int> ranked = sortedByScore(scores, matched);
    if (ranked.size() < limit)
        ranked += sortedByScore(scores, fuzzy).mid(0, limit - ranked.size());
    return ranked.mid(0, limit);
}

double matchSorterRank(const QString &value, const QString &query)
{
    QString text = stripDiacritics(value);
    QString search = stripDiacritics(query);
    if (search.size() > text.size())
        return 0;
    if (text == search)
        return 7;
    text = text.toLower();
    search = search.toLower();
    if (text == search)
        return 6;
    if (text.startsWith(search))
        return 5;
    if (text.contains(QLatin1Char(' ') + search))
        return 4;
    if (text.contains(search))
        return 3;
    if (search.size() == 1)
        return 0;

    QString acronym;
    for (const QString &word : text.split(QLatin1Char(' ')))
        for (const QString &part : word.split(QLatin1Char('-')))
            acronym += part.left(1);
    if (acronym.contains(search))
        return 2;

    int found = 0;
    auto findFrom = [&text, &found](QChar c, int from) -> int {
        for (int i = from; i < text.size(); ++i) {
            if (text[i] == c) {
                ++found;
                return i + 1;
            }
        }
        return -1;
    };
    const int first = findFrom(search[0], 0);
    if (first < 0)
        return 0;
    int last = first;
    for (int i = 1; i < search.size(); ++i) {
        last = findFrom(search[i], last);
        if (last < 0)
            return 0;
    }
    const double spread = double(last - first);
    return 1 + (1 / spread) * (double(found) / double(search.size()));
}

QList<int> rankByName(const QStringList &names, const QString &query)
{
    QVector<double> ranks(int(names.size()), 0);
    QList<int> kept;
    for (int i = 0; i < names.size(); ++i) {
        ranks[i] = matchSorterRank(names[i], query);
        if (ranks[i] >= 1)
            kept.append(i);
    }
    std::stable_sort(kept.begin(), kept.end(), [&](int a, int b) {
        if (ranks[a] != ranks[b])
            return ranks[a] > ranks[b];
        return QString::localeAwareCompare(names[a], names[b]) < 0;
    });
    return kept;
}

QList<int> rankChannels(const QString &query, const QList<ChannelCandidate> &candidates, bool voiceOnly, int limit)
{
    const QList<ChannelToken> queryTokens = channelTokens(query);
    QVector<double> scores(int(candidates.size()), 0);
    QList<int> kept;
    for (int i = 0; i < candidates.size(); ++i) {
        const ChannelCandidate &channel = candidates[i];
        QList<ChannelToken> tokens = queryTokens;
        double score = consumeBestToken(channel.name.toLower(), tokens, true);
        if (score == 0)
            continue;
        if (!tokens.isEmpty()) {
            for (const QString &context : { channel.guildName, channel.parentName }) {
                if (context.isEmpty())
                    continue;
                score += 0.5 * consumeBestToken(context.toLower(), tokens, false);
            }
            score = std::min(6.0, score);
        }
        if (score == 0 || tokens.size() > 1 || (tokens.size() == 1 && !tokens.first().isFullMatch))
            continue;
        if (!voiceOnly && channel.isVoice)
            score = std::max(score - 1, 0.5);
        if (channel.isThread) {
            if (!channel.isActiveThread)
                score -= 3;
            if (!channel.isJoinedThread)
                score -= 5;
        }
        scores[i] = score;
        kept.append(i);
    }
    return sortedByScore(scores, kept).mid(0, limit);
}

} // namespace MentionSearch
} // namespace Core
} // namespace Acheron
