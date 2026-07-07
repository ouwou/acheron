#include "MemberRepository.hpp"

#include <QJsonArray>
#include <QJsonDocument>

#include "DatabaseManager.hpp"
#include "Transaction.hpp"
#include "Core/Logging.hpp"

namespace Acheron {
namespace Storage {

MemberRepository::MemberRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

QString MemberRepository::rolesToJson(const QList<Core::Snowflake> &roles)
{
    QJsonArray arr;
    for (const auto &role : roles)
        arr.append(QString::number(static_cast<quint64>(role)));

    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

QList<Core::Snowflake> MemberRepository::rolesFromJson(const QString &json)
{
    QList<Core::Snowflake> roles;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray())
        return roles;

    for (const auto &val : doc.array())
        roles.append(static_cast<Core::Snowflake>(val.toString().toULongLong()));

    return roles;
}

void MemberRepository::saveMember(Core::Snowflake guildId, Core::Snowflake userId,
                                  const Discord::Member &member)
{
    auto db = getDb();
    saveMember(guildId, userId, member, db);
}

void MemberRepository::saveMember(Core::Snowflake guildId, Core::Snowflake userId,
                                  const Discord::Member &member, QSqlDatabase &db)
{
    QSqlQuery q(db);
    q.prepare(R"(
        INSERT OR REPLACE INTO members
        (user_id, guild_id, nick, avatar, roles, joined_at, premium_since,
         deaf, mute, flags, pending, communication_disabled_until)
        VALUES (:user_id, :guild_id, :nick, :avatar, :roles, :joined_at, :premium_since,
                :deaf, :mute, :flags, :pending, :communication_disabled_until)
    )");

    q.bindValue(":user_id", static_cast<qint64>(userId));
    q.bindValue(":guild_id", static_cast<qint64>(guildId));
    bindOptional(q, ":nick", member.nick);
    bindOptional(q, ":avatar", member.avatar);
    q.bindValue(":roles", member.roles.hasValue() ? rolesToJson(member.roles.get()) : QVariant());
    bindOptional(q, ":joined_at", member.joinedAt);
    bindOptional(q, ":premium_since", member.premiumSince);
    bindOptional(q, ":deaf", member.deaf);
    bindOptional(q, ":mute", member.mute);
    bindOptional(q, ":flags", member.flags);
    bindOptional(q, ":pending", member.pending);
    bindOptional(q, ":communication_disabled_until", member.communicationDisabledUntil);

    execLogged(q, "MemberRepository: Save member");
}

void MemberRepository::saveMembers(Core::Snowflake guildId, const QList<Discord::Member> &members)
{
    if (members.isEmpty())
        return;

    auto db = getDb();
    Transaction txn(db);
    for (const auto &member : members) {
        if (!member.user.hasValue() || !member.user->id.hasValue())
            continue;
        saveMember(guildId, member.user->id.get(), member, db);
    }
    txn.commit();
}

std::optional<Discord::Member> MemberRepository::getMember(Core::Snowflake guildId,
                                                           Core::Snowflake userId)
{
    auto db = getDb();
    QSqlQuery q(db);
    q.prepare(R"(
        SELECT nick, avatar, roles, joined_at, premium_since, deaf, mute, flags,
               pending, communication_disabled_until
        FROM members WHERE user_id = :user_id AND guild_id = :guild_id
    )");
    q.bindValue(":user_id", static_cast<qint64>(userId));
    q.bindValue(":guild_id", static_cast<qint64>(guildId));

    if (!q.exec() || !q.next()) {
        return std::nullopt;
    }

    Discord::Member member;
    if (!q.value(0).isNull())
        member.nick = q.value(0).toString();
    if (!q.value(1).isNull())
        member.avatar = q.value(1).toString();
    if (!q.value(2).isNull())
        member.roles = rolesFromJson(q.value(2).toString());
    if (!q.value(3).isNull())
        member.joinedAt = q.value(3).toDateTime();
    if (!q.value(4).isNull())
        member.premiumSince = q.value(4).toDateTime();
    if (!q.value(5).isNull())
        member.deaf = q.value(5).toBool();
    if (!q.value(6).isNull())
        member.mute = q.value(6).toBool();
    if (!q.value(7).isNull())
        member.flags = q.value(7).toInt();
    if (!q.value(8).isNull())
        member.pending = q.value(8).toBool();
    if (!q.value(9).isNull())
        member.communicationDisabledUntil = q.value(9).toDateTime();

    return member;
}

} // namespace Storage
} // namespace Acheron
