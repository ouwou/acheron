#pragma once

#include <QObject>
#include <QString>

#include <memory>

#include "AccountInfo.hpp"
#include "Discord/Client.hpp"
#include "MessageManager.hpp"
#include "UserManager.hpp"
#include "PermissionManager.hpp"

namespace Acheron {
namespace Core {

class ClientInstance : public QObject
{
    Q_OBJECT
public:
    explicit ClientInstance(const AccountInfo &info, QObject *parent = nullptr);
    ~ClientInstance() override;

    void start();
    void stop();

    [[nodiscard]] Discord::Client *discord() const;
    [[nodiscard]] MessageManager *messages() const;
    [[nodiscard]] UserManager *users() const;
    [[nodiscard]] PermissionManager *permissions() const;

    [[nodiscard]] QList<Discord::Role> getRolesForGuild(Snowflake guildId);

    [[nodiscard]] ConnectionState state() const;

    Snowflake accountId() const;
    const AccountInfo &accountInfo() const;

signals:
    void stateChanged(ConnectionState state);
    void detailsUpdated(const AccountInfo &info);

    void ready(const Discord::Ready &ready);
    void channelCreated(const Discord::ChannelCreate &event);
    void channelUpdated(const Discord::ChannelUpdate &update);
    void channelDeleted(const Discord::ChannelDelete &event);
    void membersUpdated(Snowflake guildId, const QList<Snowflake> &userIds);

private slots:
    void onChannelCreated(const Discord::ChannelCreate &event);
    void onChannelUpdated(const Discord::ChannelUpdate &event);
    void onChannelDeleted(const Discord::ChannelDelete &event);
    void onGuildMembersChunk(const Discord::GuildMembersChunk &chunk);
    void onMessagesReceived(const MessageRequestResult &result);

private:
    AccountInfo account;

    MessageManager *messageManager;
    UserManager *userManager;
    Discord::Client *client;
    PermissionManager *permissionManager;

    Storage::RoleRepository roleRepo;
    Storage::GuildRepository guildRepo;
    Storage::ChannelRepository channelRepo;
    Storage::MemberRepository memberRepo;

    // notFound members are kept in here so we dont ask for them again
    QSet<QPair<Snowflake /*guildId*/, Snowflake /*userId*/>> pendingMemberRequests;
};

} // namespace Core
} // namespace Acheron