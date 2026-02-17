#pragma once

#include <QObject>

#include "Core/Snowflake.hpp"
#include "Discord/Events.hpp"
#include "Discord/AV/VoiceClient.hpp"

namespace Acheron {
namespace Core {
namespace AV {

class VoiceManager : public QObject
{
    Q_OBJECT
public:
    explicit VoiceManager(Snowflake accountId, QObject *parent = nullptr);
    ~VoiceManager() override;

    void handleVoiceStateUpdate(const Discord::VoiceState &state);
    void handleVoiceServerUpdate(const Discord::VoiceServerUpdate &event);

    void disconnect();

    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] Snowflake currentChannelId() const { return channelId; }
    [[nodiscard]] Snowflake currentGuildId() const { return guildId; }

signals:
    void voiceConnected();
    void voiceDisconnected();
    void voiceStateChanged();

private slots:
    void onVoiceClientConnected();
    void onVoiceClientDisconnected();
    void onVoiceClientStateChanged(Discord::AV::VoiceClient::State state);

private:
    void connectToVoiceServer(const QString &endpoint, const QString &token);

private:
    Snowflake accountId;

    QString voiceSessionId;
    Snowflake guildId;
    Snowflake channelId;
    bool selfMute = false;
    bool selfDeaf = false;

    Discord::AV::VoiceClient *voiceClient = nullptr;

    // pending data: VOICE_SERVER_UPDATE may arrive before VOICE_STATE_UPDATE
    // both are needed to connect
    struct PendingConnection
    {
        QString token;
        QString endpoint;
        Snowflake guildId;
        bool hasServerUpdate = false;
        bool hasStateUpdate = false;
    };
    PendingConnection pending;
};

} // namespace AV
} // namespace Core
} // namespace Acheron
