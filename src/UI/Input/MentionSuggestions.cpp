#include "MentionSuggestions.hpp"

#include <QSet>
#include <QTimer>

#include <algorithm>
#include <tuple>

#include "Core/ClientInstance.hpp"
#include "Core/MentionSearch.hpp"
#include "Core/PermissionManager.hpp"
#include "Core/RelationshipManager.hpp"
#include "Core/Theme/Icons.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"

namespace Acheron {
namespace UI {

namespace {

constexpr int MemberSearchDelayMs = 200;
constexpr qint64 MemberSearchRepeatSeconds = 60;
constexpr int AvatarPx = 32;

namespace Search = Core::MentionSearch;

struct UserRow
{
    Discord::User user;
    QString nick;
    QString memberAvatar;
};

Core::EmojiMatch userMatch(const UserRow &row, Core::Snowflake guildId)
{
    const QString name = row.nick.isEmpty() ? row.user.getDisplayName() : row.nick;
    Core::EmojiMatch match;
    match.insertText = QLatin1Char('@') + name;
    match.displayLabel = name == row.user.username.get() ? name : QStringLiteral("%1 (%2)").arg(name, row.user.username.get());
    match.imageUrl = Discord::Cdn::effectiveAvatar(guildId, row.user.id.get(), row.memberAvatar, row.user.avatar.valueOr(), AvatarPx);
    match.mentionText = QStringLiteral("<@%1>").arg(quint64(row.user.id.get()));
    return match;
}

Core::EmojiMatch iconMatch(const QString &symbol, const QString &name, const QString &mentionText, const QString &iconName, const QColor &iconColor = {})
{
    Core::EmojiMatch match;
    match.insertText = symbol + name;
    match.displayLabel = name;
    match.mentionText = mentionText;
    match.iconName = iconName;
    match.iconColor = iconColor;
    return match;
}

bool isUsableMember(const std::optional<Discord::Member> &member)
{
    return member && member->joinedAt.hasValue() && !member->pending.valueOr(false);
}

// authors of the loaded messages, newest first, the official suggestions for a bare @ in a server
QList<UserRow> recentlyTalked(Core::ClientInstance *instance, Core::Snowflake guildId, const QVector<Discord::Message> &loadedMessages)
{
    QList<UserRow> rows;
    QSet<Core::Snowflake> seen;
    for (auto it = loadedMessages.crbegin(); it != loadedMessages.crend() && rows.size() < Search::ResultLimit; ++it) {
        if (!it->author.hasValue())
            continue;
        const Discord::User &author = it->author.get();
        if (seen.contains(author.id.get()))
            continue;
        seen.insert(author.id.get());
        if (author.isNonUserBot())
            continue;
        const std::optional<Discord::Member> member = instance->users()->getMember(guildId, author.id.get());
        if (!isUsableMember(member))
            continue;
        rows.append({ author, member->nick.valueOr(), member->avatar.valueOr() });
    }
    return rows;
}

QList<UserRow> rankedUsers(Core::ClientInstance *instance, const Discord::Channel &channel, const QString &query, const QVector<Discord::Message> &loadedMessages, MemberSearch &memberSearch)
{
    QList<UserRow> rows;
    QList<Search::UserCandidate> candidates;
    auto addCandidate = [&](const Discord::User &user, const QString &nick, const QString &memberAvatar) {
        rows.append({ user, nick, memberAvatar });
        candidates.append({ user.username.get(), nick, user.globalName.valueOr() });
    };

    const Core::Snowflake guildId = channel.guildId.valueOr(Core::Snowflake());
    if (!guildId.isValid()) {
        for (const auto &user : channel.recipients.valueOr())
            addCandidate(user, instance->relationships()->getNickname(user.id.get()), QString());
        addCandidate(instance->discord()->getMe(), QString(), QString());
        QList<UserRow> ranked;
        for (const int index : Search::rankUsers(query, candidates))
            ranked.append(rows[index]);
        return ranked;
    }

    if (query.isEmpty()) {
        QList<UserRow> recent = recentlyTalked(instance, guildId, loadedMessages);
        if (!recent.isEmpty())
            return recent;
    }

    for (const auto &member : instance->users()->getKnownMembers(guildId)) {
        if (isUsableMember(member))
            addCandidate(member.user.get(), member.nick.valueOr(), member.avatar.valueOr());
    }
    memberSearch.request(guildId, query);

    const Core::Snowflake viewedChannelId = channel.isThread() && channel.parentId.hasValue() ? channel.parentId.get() : channel.id.get();
    auto canView = [&](int index) {
        return instance->permissions()->hasChannelPermission(rows[index].user.id.get(), viewedChannelId, Discord::Permission::VIEW_CHANNEL);
    };
    QList<UserRow> ranked;
    for (const int index : Search::rankUsers(query, candidates, canView))
        ranked.append(rows[index]);
    return ranked;
}

bool isSelectableChannelType(Discord::ChannelType type)
{
    switch (type) {
    case Discord::ChannelType::GUILD_TEXT:
    case Discord::ChannelType::GUILD_NEWS:
    case Discord::ChannelType::GUILD_STORE:
    case Discord::ChannelType::GUILD_DIRECTORY:
    case Discord::ChannelType::GUILD_FORUM:
    case Discord::ChannelType::GUILD_MEDIA:
        return true;
    default:
        return false;
    }
}

bool isVocalChannelType(Discord::ChannelType type)
{
    return type == Discord::ChannelType::GUILD_VOICE || type == Discord::ChannelType::GUILD_STAGE_VOICE;
}

QString channelIcon(const Discord::Channel &channel)
{
    using namespace Core::Theme::Icons;
    if (channel.isThread())
        return Name::MessageCircle;
    switch (channel.type.get()) {
    case Discord::ChannelType::GUILD_VOICE:
    case Discord::ChannelType::GUILD_STAGE_VOICE:
        return Name::VolumeOn;
    case Discord::ChannelType::GUILD_NEWS:
        return Name::Megaphone;
    case Discord::ChannelType::GUILD_FORUM:
    case Discord::ChannelType::GUILD_MEDIA:
        return Name::MessagesSquare;
    default:
        return Name::Hash;
    }
}

} // namespace

MemberSearch::MemberSearch(QObject *parent) : QObject(parent), timer(new QTimer(this))
{
    timer->setSingleShot(true);
    timer->setInterval(MemberSearchDelayMs);
    connect(timer, &QTimer::timeout, this, &MemberSearch::send);
}

void MemberSearch::setInstance(Core::ClientInstance *newInstance)
{
    instance = newInstance;
    timer->stop();
    recentQueries.clear();
}

void MemberSearch::request(Core::Snowflake guildId, const QString &query)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = recentQueries.begin(); it != recentQueries.end();)
        it = it.value().secsTo(now) >= MemberSearchRepeatSeconds ? recentQueries.erase(it) : std::next(it);
    const QString key = QStringLiteral("%1:%2").arg(quint64(guildId)).arg(query);
    if (recentQueries.contains(key))
        return;
    // like the official client, a query counts as asked even when a later keystroke cancels it before it is sent
    recentQueries.insert(key, now);
    pendingGuildId = guildId;
    pendingQuery = query;
    timer->start();
}

void MemberSearch::send()
{
    if (instance)
        instance->discord()->queryGuildMembers(pendingGuildId, pendingQuery.toLower(), Core::MentionSearch::ResultLimit, true);
}

namespace MentionSuggestions {

QList<Core::EmojiMatch> forMention(Core::ClientInstance *instance, Core::Snowflake channelId, const QString &query, const QVector<Discord::Message> &loadedMessages, MemberSearch &memberSearch)
{
    const std::optional<Discord::Channel> channel = instance->getChannel(channelId);
    if (!channel)
        return {};
    const std::optional<Discord::Channel> parent = channel->isThread() && channel->parentId.hasValue() ? instance->getChannel(channel->parentId.get()) : std::nullopt;
    const Discord::Channel &base = parent ? *parent : *channel;
    const Core::Snowflake guildId = base.guildId.valueOr(Core::Snowflake());

    QList<Core::EmojiMatch> users;
    for (const UserRow &row : rankedUsers(instance, *channel, query, loadedMessages, memberSearch))
        users.append(userMatch(row, guildId));
    if (!guildId.isValid())
        return users;

    const bool canMentionEveryone = instance->permissions()->hasChannelPermission(instance->accountId(), base.id.get(), Discord::Permission::MENTION_EVERYONE);
    const QString lowerQuery = query.toLower();
    int count = int(users.size());

    QList<Core::EmojiMatch> roles;
    if (count < Search::ResultLimit) {
        QList<Discord::Role> candidates;
        QStringList names;
        for (const auto &role : instance->getRolesForGuild(guildId)) {
            if (role.id.get() == guildId || !(role.mentionable.valueOr(false) || canMentionEveryone) || !Search::isSubsequence(lowerQuery, role.name.get().toLower()))
                continue;
            candidates.append(role);
            names.append(role.name.get());
        }
        for (const int index : Search::rankByName(names, query).mid(0, Search::ResultLimit - count)) {
            const Discord::Role &role = candidates[index];
            roles.append(iconMatch(QStringLiteral("@"), role.name.get(), QStringLiteral("<@&%1>").arg(quint64(role.id.get())), Core::Theme::Icons::Name::AtSign, role.getColor()));
        }
        count += int(roles.size());
    }

    QList<Core::EmojiMatch> globals;
    if (canMentionEveryone) {
        for (const QString special : { QStringLiteral("everyone"), QStringLiteral("here") }) {
            if (count < Search::ResultLimit && Search::isSubsequence(lowerQuery, special)) {
                globals.append(iconMatch(QStringLiteral("@"), special, QLatin1Char('@') + special, Core::Theme::Icons::Name::AtSign));
                ++count;
            }
        }
    }
    return users + globals + roles;
}

QList<Core::EmojiMatch> forChannel(Core::ClientInstance *instance, Core::Snowflake channelId, const QString &query)
{
    const std::optional<Discord::Channel> current = instance->getChannel(channelId);
    if (!current || !current->guildId.hasValue())
        return {};
    const Core::Snowflake guildId = current->guildId.get();

    const bool voiceOnly = query.startsWith(QLatin1Char('!'));
    const QString search = voiceOnly ? query.mid(1) : query;

    QList<Discord::Channel> all = instance->getGuildChannels(guildId);
    QHash<Core::Snowflake, Discord::Channel> byId;
    for (const auto &channel : all)
        byId.insert(channel.id.get(), channel);
    auto nameOf = [&byId](const std::optional<Core::Snowflake> &id) -> QString {
        if (!id)
            return {};
        const auto it = byId.constFind(*id);
        return it != byId.constEnd() ? it.value().name.valueOr() : QString();
    };
    auto parentOf = [](const Discord::Channel &channel) -> std::optional<Core::Snowflake> {
        if (channel.parentId.hasValue() && channel.parentId.get().isValid())
            return channel.parentId.get();
        return std::nullopt;
    };
    auto canAccess = [&](const Discord::Channel &channel) {
        const Core::Snowflake me = instance->accountId();
        if (!instance->permissions()->hasChannelPermission(me, channel.id.get(), Discord::Permission::VIEW_CHANNEL))
            return false;
        return !channel.isVoice() || instance->permissions()->hasChannelPermission(me, channel.id.get(), Discord::Permission::CONNECT);
    };

    // equal scores keep this order, the sidebar's, like the official channel store
    auto sidebarOrder = [&](const Discord::Channel &channel) {
        const std::optional<Core::Snowflake> parent = parentOf(channel);
        const auto it = parent ? byId.constFind(*parent) : byId.constEnd();
        const int categoryPosition = it != byId.constEnd() ? it.value().position.valueOr(0) : -1;
        return std::make_tuple(categoryPosition, quint64(parent.value_or(Core::Snowflake(0))), channel.position.valueOr(0), quint64(channel.id.get()));
    };

    QList<Discord::Channel> channels;
    for (const auto &channel : all) {
        if (channel.isThread() || !channel.type.hasValue())
            continue;
        const bool wanted = voiceOnly ? isVocalChannelType(channel.type.get()) : isSelectableChannelType(channel.type.get());
        if (wanted && canAccess(channel))
            channels.append(channel);
    }
    std::stable_sort(channels.begin(), channels.end(), [&](const Discord::Channel &a, const Discord::Channel &b) { return sidebarOrder(a) < sidebarOrder(b); });

    if (!voiceOnly) {
        for (const auto &thread : instance->joinedActiveThreads(guildId)) {
            const std::optional<Core::Snowflake> parent = parentOf(thread);
            const auto parentChannel = parent ? byId.constFind(*parent) : byId.constEnd();
            if (parentChannel != byId.constEnd() && canAccess(parentChannel.value()))
                channels.append(thread);
        }
    }

    const std::optional<Discord::Guild> guild = instance->getGuild(guildId);
    const QString guildName = guild ? guild->name.valueOr() : QString();
    QList<Search::ChannelCandidate> candidates;
    for (const auto &channel : channels) {
        Search::ChannelCandidate candidate;
        candidate.name = channel.name.valueOr();
        candidate.guildName = guildName;
        candidate.parentName = nameOf(parentOf(channel));
        candidate.isVoice = channel.isVoice();
        candidate.isThread = channel.isThread();
        candidate.isActiveThread = candidate.isThread && !channel.isArchived();
        candidate.isJoinedThread = candidate.isThread && instance->isThreadJoined(channel.id.get());
        candidates.append(candidate);
    }

    QList<Core::EmojiMatch> matches;
    for (const int index : Search::rankChannels(search, candidates, voiceOnly)) {
        const Discord::Channel &channel = channels[index];
        matches.append(iconMatch(QStringLiteral("#"), channel.name.valueOr(), QStringLiteral("<#%1>").arg(quint64(channel.id.get())), channelIcon(channel)));
    }
    return matches;
}

} // namespace MentionSuggestions

} // namespace UI
} // namespace Acheron
