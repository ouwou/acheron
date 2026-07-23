#pragma once

#include <QSqlDatabase>
#include <optional>

#include "BaseRepository.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

namespace Acheron {
namespace Storage {

class MemberRepository : public BaseRepository
{
public:
    MemberRepository(Core::Snowflake accountId);

    void saveMember(Core::Snowflake guildId, Core::Snowflake userId, const Discord::Member &member);
    void saveMember(Core::Snowflake guildId, Core::Snowflake userId, const Discord::Member &member,
                    QSqlDatabase &db);
    void saveMembers(Core::Snowflake guildId, const QList<Discord::Member> &members);
    void deleteMembersForGuild(Core::Snowflake guildId, QSqlDatabase &db);

    std::optional<Discord::Member> getMember(Core::Snowflake guildId, Core::Snowflake userId);

private:
    static QString rolesToJson(const QList<Core::Snowflake> &roles);
    static QList<Core::Snowflake> rolesFromJson(const QString &json);
};

} // namespace Storage
} // namespace Acheron
