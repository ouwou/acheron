#pragma once

#include <QList>
#include <QString>

#include <functional>

namespace Acheron {
namespace Core {

// Ranking for the chat input's @ and # autocomplete, ported from the official client's AutocompleteUtils.
// Every function returns indices into the candidate list, best first; equal scores keep the candidates' order.
namespace MentionSearch {

inline constexpr int ResultLimit = 10;

QString stripDiacritics(const QString &text);
// look-alike characters replaced, then lowercased, as the official client folds names
QString foldConfusables(const QString &text);
bool isSubsequence(const QString &needle, const QString &haystack);

struct UserCandidate
{
    QString username;
    QString nick; // guild nickname, or friend nickname in private channels
    QString globalName;
};

// accept, when set, drops candidates the official client filters out (members who can't see the channel)
QList<int> rankUsers(const QString &query, const QList<UserCandidate> &candidates, const std::function<bool(int index)> &accept = {}, int limit = ResultLimit);

// the official client ranks roles with match-sorter on the role name
double matchSorterRank(const QString &value, const QString &query);
QList<int> rankByName(const QStringList &names, const QString &query);

struct ChannelCandidate
{
    QString name;
    QString guildName;
    QString parentName;
    bool isVoice = false;
    bool isThread = false;
    bool isActiveThread = false;
    bool isJoinedThread = false;
};

QList<int> rankChannels(const QString &query, const QList<ChannelCandidate> &candidates, bool voiceOnly, int limit = ResultLimit);

} // namespace MentionSearch

} // namespace Core
} // namespace Acheron
