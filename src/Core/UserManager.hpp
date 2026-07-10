#pragma once

#include <QCache>
#include <QObject>
#include <QHash>
#include <optional>

#include "Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Storage/UserRepository.hpp"
#include "Storage/MemberRepository.hpp"

namespace Acheron {
namespace Core {

struct MemberKey
{
    Snowflake guildId;
    Snowflake userId;

    bool operator==(const MemberKey &other) const
    {
        return guildId == other.guildId && userId == other.userId;
    }
};

inline size_t qHash(const MemberKey &key, size_t seed = 0)
{
    return qHashMulti(seed, key.guildId, key.userId);
}

class UserManager : public QObject
{
    Q_OBJECT
public:
    explicit UserManager(Snowflake accountId, QObject *parent = nullptr);
    ~UserManager() override;

    [[nodiscard]] std::optional<Discord::User> getUser(Snowflake userId);
    [[nodiscard]] std::optional<Discord::Member> getMember(Snowflake guildId, Snowflake userId);

    [[nodiscard]] std::optional<QList<Snowflake>> getMemberRoles(Snowflake guildId, Snowflake userId);

    [[nodiscard]] QString getDisplayName(Snowflake userId, Snowflake guildId = Snowflake::Invalid);

    void saveUser(const Discord::User &user);
    void saveUsers(const QList<Discord::User> &users);
    void saveMember(Snowflake guildId, Snowflake userId, const Discord::Member &member);
    void saveMembers(Snowflake guildId, const QList<Discord::Member> &members);

    void saveMemberWithUser(Snowflake guildId, const Discord::Member &member);

    void loadNotesFromReady(const QHash<Snowflake, QString> &notes);
    void setCachedNote(Snowflake userId, const QString &note);
    [[nodiscard]] std::optional<QString> getCachedNote(Snowflake userId) const;

signals:
    void noteChanged(Snowflake userId);

private:
    QCache<Snowflake, Discord::User> userCache;
    QCache<MemberKey, Discord::Member> memberCache;

    QHash<Snowflake, QString> notes;

    Storage::UserRepository userRepo;
    Storage::MemberRepository memberRepo;
};

} // namespace Core
} // namespace Acheron
