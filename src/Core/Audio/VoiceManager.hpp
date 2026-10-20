#pragma once

#include <QObject>
#include <QThread>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <opus.h>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Audio/IAudioBackend.hpp"
#include "Discord/Events.hpp"
#include "Discord/Voice/VoiceClient.hpp"

namespace Acheron {
namespace Core {

class CallVideo;

namespace Audio {

class AudioPipeline;
class VoiceConnection;

struct VoiceParticipant
{
    Snowflake userId;
    bool selfMute = false;
    bool selfDeaf = false;
    bool serverMute = false;
    bool serverDeaf = false;
    bool suppress = false;
    bool selfVideo = false;
    bool selfStream = false;
    bool speaking = false;
};

class VoiceManager : public QObject
{
    Q_OBJECT
public:
    explicit VoiceManager(Snowflake accountId, const ProxyConfig &proxy, QObject *parent = nullptr);
    ~VoiceManager() override;

    void handleVoiceStateUpdate(const Discord::VoiceState &state);
    void handleVoiceServerUpdate(const Discord::VoiceServerUpdate &event);

    // The id of the gateway session this client is, from READY. A voice state for this account
    // carrying any other session id belongs to another of the account's devices.
    void setGatewaySessionId(const QString &sessionId);

    // A private call ended: everyone the gateway listed in it is gone.
    void forgetChannelVoiceStates(Snowflake channelId);

    void disconnect();

    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] Discord::Voice::VoiceClient::State clientState() const;
    [[nodiscard]] Snowflake currentChannelId() const { return channelId; }
    [[nodiscard]] Snowflake currentGuildId() const { return guildId; }
    // The account's own voice state as the server last gave it for this client's connection.
    [[nodiscard]] bool isSelfMuted() const { return selfMute; }
    [[nodiscard]] bool isSelfDeafened() const { return selfDeaf; }
    [[nodiscard]] bool isSelfVideo() const { return selfVideo; }

    // The voice connection's client, for what else is sent on that connection (the account's
    // camera). It lives on the connection's thread and goes when the connection does:
    // rtcClientCreated announces each one before it starts, and rtcClientClosing its end.
    [[nodiscard]] Discord::Voice::VoiceClient *rtcClient() const { return voiceClient; }

    // The codecs voice connections offer to send video with, which is the camera: those this
    // machine has turned out to encode, as far as the camera can be sent with them. A
    // connection that is already up is told now.
    void setEncodableVideoCodecs(const QList<Discord::Voice::VideoCodec> &codecs);

    void setInputGain(float gain);
    void setOutputVolume(float volume);
    void setUserVolume(Snowflake userId, float volume);
    void setVadThreshold(float threshold);

    void setOpusApplication(int application);
    void setOpusBitrate(int bitrate);
    void setOpusComplexity(int complexity);
    void setOpusSignalType(int signalType);
    void setOpusFec(bool enabled);
    void setOpusPacketLossPercent(int percent);

    void setNoiseSuppressionEnabled(bool enabled);
    void setUseRnnoiseVad(bool enabled);

    [[nodiscard]] int opusApplication() const { return cachedOpusApplication; }
    [[nodiscard]] int opusBitrate() const { return cachedOpusBitrate; }
    [[nodiscard]] int opusComplexity() const { return cachedOpusComplexity; }
    [[nodiscard]] int opusSignalType() const { return cachedOpusSignalType; }
    [[nodiscard]] bool opusFec() const { return cachedOpusFec; }
    [[nodiscard]] int opusPacketLossPercent() const { return cachedOpusPacketLossPercent; }
    [[nodiscard]] bool noiseSuppression() const { return cachedNoiseSuppression; }
    [[nodiscard]] bool rnnoiseVad() const { return cachedUseRnnoiseVad; }

    [[nodiscard]] QList<AudioDeviceInfo> availableInputDevices() const;
    [[nodiscard]] QList<AudioDeviceInfo> availableOutputDevices() const;
    [[nodiscard]] QByteArray currentInputDevice() const { return currentInputDeviceId; }
    [[nodiscard]] QByteArray currentOutputDevice() const { return currentOutputDeviceId; }
    void setInputDevice(const QByteArray &deviceId);
    void setOutputDevice(const QByteArray &deviceId);

    [[nodiscard]] QList<VoiceParticipant> currentParticipants() const;
    [[nodiscard]] const VoiceParticipant *participant(Snowflake userId) const;
    [[nodiscard]] int channelVoiceUserCount(Snowflake channelId) const;
    [[nodiscard]] QList<Snowflake> channelVoiceUsers(Snowflake channelId) const;
    [[nodiscard]] std::optional<Discord::VoiceState> voiceStateForUser(Snowflake userId) const;

    // Mixes the audio of another RTC connection, such as a watched stream, into this voice
    // connection's output under its own volume. `source` is any non-zero number unique to it.
    void attachAudioSource(quint32 source, Discord::Voice::VoiceClient *client);
    void detachAudioSource(quint32 source);
    void setAudioSourceVolume(quint32 source, float volume);

#ifdef ACHERON_HAVE_VIDEO
    // The cameras of the call this account is in.
    [[nodiscard]] CallVideo *video() const { return callVideo; }
#endif

    void setUserMuted(Snowflake userId, bool muted);
    [[nodiscard]] bool isUserMuted(Snowflake userId) const;

    [[nodiscard]] bool isDaveEnabled() const;
    [[nodiscard]] const QString &privacyCode() const { return cachedPrivacyCode; }
    void requestVerificationCode(Snowflake targetUserId, std::function<void(const QString &)> callback);

signals:
    void voiceConnected();
    void voiceDisconnected();
    void rtcClientCreated(Discord::Voice::VoiceClient *client);
    // The client can still be called, and is stopped and destroyed straight after.
    void rtcClientClosing();
    // Another session of this account joined voice while this client was connected, which ends
    // this client's connection without it having asked to leave.
    void takenOverByAnotherSession(Snowflake channelId, Snowflake guildId);
    void voiceStateChanged();
    void audioLevelChanged(float rms);
    void speakingChanged(bool speaking);
    void devicesChanged();

    void participantJoined(Snowflake userId);
    void participantLeft(Snowflake userId);
    void participantUpdated(Snowflake userId);
    void participantSpeakingChanged(Snowflake userId, bool speaking);
    void channelVoiceMemberChanged(Snowflake channelId, Snowflake userId, bool joined);
    void participantVoiceStateChanged(Snowflake channelId, Snowflake userId);
    void participantsCleared();
    void userAudioLevelChanged(Snowflake userId, float rms);
    void privacyCodeChanged(const QString &code);

private slots:
    void onVoiceClientConnected();
    void onVoiceClientDisconnected();
    void onVoiceClientStateChanged(Discord::Voice::VoiceClient::State state);
    void onDevicesChanged(const QList<AudioDeviceInfo> &inputs, const QList<AudioDeviceInfo> &outputs);

private:
    void connectToVoiceServer(const QString &endpoint, const QString &token);
    void stopVoiceThread();
    void populateParticipantsFromCache();
    IAudioBackend *backend() const;

private:
    Snowflake accountId;
    ProxyConfig proxy;

    QString gatewaySessionId;
    QString voiceSessionId;
    Snowflake guildId;
    Snowflake channelId;
    bool selfMute = false;
    bool selfDeaf = false;
    // Set by the server while this account has a stream up; the client never asks for it.
    bool selfStream = false;
    bool selfVideo = false;
    QList<Discord::Voice::VideoCodec> encodableVideoCodecs;

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

    int cachedOpusApplication = OPUS_APPLICATION_VOIP;
    int cachedOpusBitrate = 64000;
    int cachedOpusComplexity = 5;
    int cachedOpusSignalType = OPUS_SIGNAL_VOICE;
    bool cachedOpusFec = true;
    int cachedOpusPacketLossPercent = 0;
    bool cachedNoiseSuppression = true;
#ifdef ACHERON_HAVE_RNNOISE
    bool cachedUseRnnoiseVad = true;
#else
    bool cachedUseRnnoiseVad = false;
#endif
    QList<AudioDeviceInfo> cachedInputDevices;
    QList<AudioDeviceInfo> cachedOutputDevices;

    QHash<Snowflake, VoiceParticipant> participants;
    QSet<Snowflake> mutedUsers;
    QHash<Snowflake, Discord::VoiceState> knownVoiceStates;
    QString cachedPrivacyCode;

    VoiceConnection *voiceConnection = nullptr;
    Discord::Voice::VoiceClient *voiceClient = nullptr;
    AudioPipeline *audioPipeline = nullptr;

    struct AudioSource
    {
        QPointer<Discord::Voice::VoiceClient> client;
        float volume = 1.0f;
    };
    // Kept apart from the pipeline, which is replaced whenever the voice connection is.
    QHash<quint32, AudioSource> audioSources;
    void feedAudioSourceToPipeline(quint32 source);
    CallVideo *callVideo = nullptr;
    unsigned int voiceGeneration = 0;

    mutable std::unique_ptr<IAudioBackend> audioBackend;
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
