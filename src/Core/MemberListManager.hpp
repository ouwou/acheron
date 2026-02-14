#pragma once

#include <QObject>
#include <QList>
#include <QColor>
#include <QString>
#include <QTimer>

#include "Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"

namespace Acheron {

namespace Storage {
class ChannelRepository;
class RoleRepository;
} // namespace Storage

namespace Core {

struct MemberListItem
{
    enum class Type {
        Group,
        Member,
        Placeholder
    };
    Type type = Type::Placeholder;

    QString groupId; // role snowflake string, "online", or "offline"
    QString groupName;
    int groupCount = 0;
    QColor groupColor;

    Discord::Member member;
    Snowflake userId;
    QString displayName;
    QColor roleColor;
};

struct ListData
{
    QHash<int, MemberListItem> items;
    QList<Discord::GuildMemberListUpdate::Group> groups;
};

struct GuildListState
{
    QHash<QString, ListData> lists; // listId -> items + groups
    QHash<QString, Discord::Role> roleCache;
    int memberCount = 0;
    int onlineCount = 0;
};

class MemberListManager : public QObject
{
    Q_OBJECT
public:
    explicit MemberListManager(Storage::ChannelRepository &channelRepo,
                               Storage::RoleRepository &roleRepo,
                               QObject *parent = nullptr);

    void setActiveChannel(Snowflake guildId, Snowflake channelId);
    void clear();

    void handleMemberListUpdate(const Discord::GuildMemberListUpdate &update);

    void handleRoleCreated(Snowflake guildId, const Discord::Role &role);
    void handleRoleUpdated(Snowflake guildId, const Discord::Role &role);
    void handleRoleDeleted(Snowflake guildId, Snowflake roleId);

    void updateSubscriptionRange(int firstVisible, int lastVisible);

    // virtual row count
    [[nodiscard]] int totalItemCount() const;

    // virtual index
    [[nodiscard]] const MemberListItem *itemAt(int index) const;

    [[nodiscard]] bool isLoaded(int index) const;
    [[nodiscard]] Snowflake currentGuildId() const { return activeGuildId; }
    [[nodiscard]] Snowflake currentChannelId() const { return activeChannelId; }
    [[nodiscard]] int totalMemberCount() const;
    [[nodiscard]] int onlineCount() const;
    [[nodiscard]] const QList<QPair<int, int>> &currentRanges() const { return ranges; }

    static QString computeListId(const QList<Discord::PermissionOverwrite> &overwrites, Discord::Permissions everyonePermissions);

signals:
    void listAboutToReset();
    void listReset();

    void subscriptionRequested(Snowflake guildId, Snowflake channelId, const QList<QPair<int, int>> &ranges);

private:
    GuildListState *activeGuildState();
    const GuildListState *activeGuildState() const;
    ListData *activeListData();
    const ListData *activeListData() const;

    MemberListItem syncItemToListItem(const Discord::GuildMemberListUpdate::SyncItem &syncItem,
                                      const GuildListState &guildState, const ListData &listData);
    void resolveGroupInfo(MemberListItem &item, const GuildListState &guildState,
                          const ListData &listData);
    void resolveMemberInfo(MemberListItem &item, const GuildListState &guildState);
    void processSync(const Discord::GuildMemberListUpdate::ListOp &op, ListData &listData,
                     const GuildListState &guildState);
    void processInsert(const Discord::GuildMemberListUpdate::ListOp &op, ListData &listData,
                       const GuildListState &guildState);
    void processUpdate(const Discord::GuildMemberListUpdate::ListOp &op, ListData &listData,
                       const GuildListState &guildState);
    void processDelete(const Discord::GuildMemberListUpdate::ListOp &op, ListData &listData);
    void processInvalidate(const Discord::GuildMemberListUpdate::ListOp &op, ListData &listData);

    void evictUnsubscribedItems(const QList<QPair<int, int>> &oldRanges,
                                const QList<QPair<int, int>> &newRanges);
    void applyAndSendRanges(const QList<QPair<int, int>> &newRanges);
    void flushPendingRanges();
    static bool indexInRanges(int index, const QList<QPair<int, int>> &ranges);

    Storage::ChannelRepository &channelRepo;
    Storage::RoleRepository &roleRepo;

    QHash<Snowflake, GuildListState> guildStates;

    Snowflake activeGuildId;
    Snowflake activeChannelId;
    QString listId; // active list

    QList<QPair<int, int>> ranges;

    // avoid spamming
    QTimer responseTimer;
    bool awaitingResponse = false;
    bool hasPendingRanges = false;
    QList<QPair<int, int>> pendingRanges;
};

} // namespace Core
} // namespace Acheron
