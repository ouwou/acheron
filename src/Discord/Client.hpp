#pragma once

#include <QObject>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrlQuery>

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Enums.hpp"

#include "Proto/UserSettings.hpp"

#include "Gateway.hpp"
#include "HttpClient.hpp"
#include "ClientIdentity.hpp"

using Acheron::Core::Snowflake;

namespace Acheron {
namespace Discord {

class Client : public QObject
{
    Q_OBJECT
public:
    enum class MessageLoadType {
        Latest,
        History,
        Future,
        Jump,
        Created,
    };
    Q_ENUM(MessageLoadType)

    explicit Client(const QString &token, const QString &gatewayUrl, const QString &baseUrl,
                    QObject *parent = nullptr);

    void start();
    void stop();

    [[nodiscard]] Core::ConnectionState getState() const;

    using MessagesCallback = std::function<void(const Core::Result<QList<Message>> &)>;
    void fetchLatestMessages(Snowflake channelId, int limit, MessagesCallback callback);
    void fetchHistory(Snowflake channelId, Snowflake beforeId, int limit,
                      MessagesCallback callback);

    void sendMessage(Snowflake channelId, const QString &content, const QString &nonce,
                     Snowflake replyToMessageId = Snowflake::Invalid);
    void editMessage(Snowflake channelId, Snowflake messageId, const QString &content);
    void deleteMessage(Snowflake channelId, Snowflake messageId);
    void pinMessage(Snowflake channelId, Snowflake messageId);
    void unpinMessage(Snowflake channelId, Snowflake messageId);

    void addReaction(Snowflake channelId, Snowflake messageId, const QString &emoji,
                     bool isBurst = false);
    void removeReaction(Snowflake channelId, Snowflake messageId, const QString &emoji,
                        bool isBurst = false);

    struct AckEntry
    {
        Snowflake channelId;
        Snowflake messageId;
        int readStateType = 0;
    };

    void ackMessage(Snowflake channelId, Snowflake messageId, int flags, int lastViewed);
    void ackBulk(const QList<AckEntry> &entries);

    void sendVoiceStateUpdate(Snowflake guildId, Snowflake channelId, bool selfMute, bool selfDeaf);

    void debugForceReconnect();

    void subscribeToGuildChannel(Snowflake guildId, Snowflake channelId,
                                 const QList<QPair<int, int>> &ranges);
    void ensureSubscriptionByChannel(Snowflake channelId);
    void requestGuildMembers(Snowflake guildId, const QList<Snowflake> &userIds);

    [[nodiscard]] Snowflake getGuildIdForChannel(Snowflake channelId) const;

    [[nodiscard]] const Proto::PreloadedUserSettings &getSettings() const;
    [[nodiscard]] const User &getMe() const;

signals:
    void stateChanged(Core::ConnectionState state);
    void ready(const Ready &data);
    void readySupplemental(const ReadySupplemental &data);
    void messageCreated(const Message &msg);
    void messageUpdated(const Message &msg);
    void messageDeleted(const MessageDelete &event);
    void typingStart(const TypingStart &event);
    void channelCreated(const ChannelCreate &event);
    void channelUpdated(const ChannelUpdate &event);
    void channelDeleted(const ChannelDelete &event);
    void guildMembersChunk(const GuildMembersChunk &chunk);
    void guildMemberUpdated(const GuildMemberUpdate &event);
    void guildRoleCreated(const GuildRoleCreate &event);
    void guildRoleUpdated(const GuildRoleUpdate &event);
    void guildRoleDeleted(const GuildRoleDelete &event);
    void messageAcked(const MessageAck &event);
    void messageReactionAdd(const MessageReactionAdd &event);
    void messageReactionAddMany(const MessageReactionAddMany &event);
    void messageReactionRemove(const MessageReactionRemove &event);
    void messageReactionRemoveAll(const MessageReactionRemoveAll &event);
    void messageReactionRemoveEmoji(const MessageReactionRemoveEmoji &event);
    void userGuildSettingsUpdated(const UserGuildSettings &settings);
    void guildMemberListUpdate(const GuildMemberListUpdate &event);
    void voiceStateUpdated(const VoiceState &event);
    void voiceServerUpdated(const VoiceServerUpdate &event);
    void messageSendFailed(const QString &nonce, const QString &error);

    void reconnecting(int attempt, int maxAttempts);
    void errorOccurred(const QString &errorStr);

private slots:
    void onConnected();
    void onDisconnected(CloseCode code, const QString &reason);

    void onGatewayReady(const Ready &data);
    void onGatewayReadySupplemental(const ReadySupplemental &data);
    void onGatewayMessageCreate(const Message &msg);
    void onGatewayMessageUpdate(const Message &msg);
    void onGatewayMessageDelete(const MessageDelete &event);
    void onGatewayChannelCreate(const ChannelCreate &event);
    void onGatewayChannelUpdate(const ChannelUpdate &event);
    void onGatewayChannelDelete(const ChannelDelete &event);
    void onGatewayGuildRoleCreate(const GuildRoleCreate &event);
    void onGatewayGuildRoleUpdate(const GuildRoleUpdate &event);
    void onGatewayGuildRoleDelete(const GuildRoleDelete &event);

private:
    void setState(Core::ConnectionState state);

private:
    Core::ConnectionState state = Core::ConnectionState::Disconnected;

    QString baseUrl;
    QString token;

    ClientIdentity identity;
    HttpClient *httpClient;
    Gateway *gateway;

    QHash<Snowflake, Snowflake> channelToGuild; // todo prob move this somewhere or just a cache
    QSet<Snowflake> subscribedGuilds;

    Proto::PreloadedUserSettings settings;
    User me;
};

} // namespace Discord
} // namespace Acheron