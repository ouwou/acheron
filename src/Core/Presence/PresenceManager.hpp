#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <optional>

#include "PresenceBadge.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"
#include "Core/Snowflake.hpp"
#include "Proto/UserSettings.hpp"

namespace Acheron {
namespace Core {

struct PresenceEntry
{
    Discord::StatusType status = Discord::StatusType::OFFLINE;
    Discord::ClientStatus clientStatus;
    QList<Discord::Activity> sortedActivities;
    qint64 receivedAtMs = 0;
};

class PresenceManager : public QObject
{
    Q_OBJECT
public:
    explicit PresenceManager(QObject *parent = nullptr);

    void setSelfUserId(Snowflake userId);
    void reset();

    void loadMergedPresences(const Discord::MergedPresences &merged,
                             const QList<Snowflake> &guildIdsInOrder);
    void applyProtoStatus(const Proto::PreloadedUserSettings &settings);

    [[nodiscard]] PresenceBadge badge(Snowflake userId, Snowflake guildId = {}) const;
    [[nodiscard]] QList<Discord::Activity> activities(Snowflake userId, Snowflake guildId = {}) const;
    [[nodiscard]] const Discord::Activity *primaryActivity(Snowflake userId, Snowflake guildId = {}) const;

public slots:
    void onPresenceUpdate(const Discord::Presence &presence);
    void onPresencesReplace(const QList<Discord::Presence> &presences);
    void onSessionsReplace(const QList<Discord::UserSession> &sessions);
    void onGuildCreated(const Discord::GatewayGuild &guild);
    void onGuildRemoved(Snowflake guildId);
    void onGuildMemberListUpdate(const Discord::GuildMemberListUpdate &update);
    void onGuildMembersChunk(const Discord::GuildMembersChunk &chunk);

signals:
    void presencesChanged(const QList<Snowflake> &userIds);

private:
    bool apply(const Discord::Presence &presence, Snowflake guildId);
    void ingestMember(const Discord::Member &member, Snowflake guildId);
    void dropSlot(Snowflake guildId);
    void rebuildSelfActivities(QList<Discord::Activity> sessionActivities);
    void markDirty(Snowflake userId);
    void flushDirty();

    [[nodiscard]] bool hidesGames(Snowflake userId) const;
    [[nodiscard]] const PresenceEntry *resolve(Snowflake userId, Snowflake guildId) const;
    [[nodiscard]] const PresenceEntry *newest(Snowflake userId) const;

    QHash<Snowflake, QHash<Snowflake, PresenceEntry>> presences;

    Snowflake selfUserId;
    PresenceEntry selfPresence;
    std::optional<Discord::Activity> selfCustomStatus;
    bool sessionsReplaceSeen = false;
    bool showCurrentGame = true;

    QSet<Snowflake> dirtyUsers;
    QTimer flushTimer;
};

} // namespace Core
} // namespace Acheron
