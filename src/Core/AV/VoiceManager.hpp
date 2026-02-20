#pragma once

#include <QObject>
#include <QThread>
#include <QList>

#include <memory>

#include "Core/Snowflake.hpp"
#include "Core/AV/IAudioBackend.hpp"
#include "Discord/Events.hpp"
#include "Discord/AV/VoiceClient.hpp"

namespace Acheron {
namespace Core {
namespace AV {

class AudioPipeline;

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
    [[nodiscard]] Discord::AV::VoiceClient::State clientState() const;
    [[nodiscard]] Snowflake currentChannelId() const { return channelId; }
    [[nodiscard]] Snowflake currentGuildId() const { return guildId; }

    void setInputGain(float gain);
    void setOutputVolume(float volume);
    void setUserVolume(Snowflake userId, float volume);
    void setVadThreshold(float threshold);

    [[nodiscard]] QList<AudioDeviceInfo> availableInputDevices() const;
    [[nodiscard]] QList<AudioDeviceInfo> availableOutputDevices() const;
    [[nodiscard]] QByteArray currentInputDevice() const { return currentInputDeviceId; }
    [[nodiscard]] QByteArray currentOutputDevice() const { return currentOutputDeviceId; }
    void setInputDevice(const QByteArray &deviceId);
    void setOutputDevice(const QByteArray &deviceId);

signals:
    void voiceConnected();
    void voiceDisconnected();
    void voiceStateChanged();
    void audioLevelChanged(float rms);
    void speakingChanged(bool speaking);
    void devicesChanged();

private slots:
    void onVoiceClientConnected();
    void onVoiceClientDisconnected();
    void onVoiceClientStateChanged(Discord::AV::VoiceClient::State state);
    void onDevicesChanged(const QList<AudioDeviceInfo> &inputs, const QList<AudioDeviceInfo> &outputs);

private:
    void connectToVoiceServer(const QString &endpoint, const QString &token);
    void stopVoiceThread();

private:
    Snowflake accountId;

    QString voiceSessionId;
    Snowflake guildId;
    Snowflake channelId;
    bool selfMute = false;
    bool selfDeaf = false;

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

    QByteArray currentInputDeviceId;
    QByteArray currentOutputDeviceId;
    QList<AudioDeviceInfo> cachedInputDevices;
    QList<AudioDeviceInfo> cachedOutputDevices;

    QThread *voiceThread = nullptr;
    Discord::AV::VoiceClient *voiceClient = nullptr;
    AudioPipeline *audioPipeline = nullptr;
    QList<QMetaObject::Connection> voiceConnections;
    unsigned int voiceGeneration = 0;

    std::unique_ptr<IAudioBackend> audioBackend;
};

} // namespace AV
} // namespace Core
} // namespace Acheron
