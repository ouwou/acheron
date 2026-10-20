#include "VoiceManager.hpp"
#include "AudioPipeline.hpp"
#include "VoiceConnection.hpp"

#include "Core/Logging.hpp"
#ifdef ACHERON_HAVE_VIDEO
#include "Core/Media/CodecSupport.hpp"
#include "Core/Video/CallVideo.hpp"
#include "Core/Video/SendQuality.hpp"
#endif

#ifdef Q_OS_WIN
#include <objbase.h>
#endif

namespace Acheron {
namespace Core {
namespace Audio {

VoiceManager::VoiceManager(Snowflake accountId, const ProxyConfig &proxy, QObject *parent)
    : QObject(parent), accountId(accountId), proxy(proxy)
{
#ifdef ACHERON_HAVE_VIDEO
    callVideo = new CallVideo(this);
#endif
}

VoiceManager::~VoiceManager()
{
    stopVoiceThread();
}

IAudioBackend *VoiceManager::backend() const
{
    if (!audioBackend) {
        audioBackend = IAudioBackend::create();
        connect(audioBackend.get(), &IAudioBackend::devicesChanged, this, &VoiceManager::onDevicesChanged);
    }
    return audioBackend.get();
}

void VoiceManager::handleVoiceStateUpdate(const Discord::VoiceState &state)
{
    Snowflake userId = state.userId.get();
    bool isMe = (userId == accountId);
    bool leftVoice = state.channelId.isNull() || !state.channelId.get().isValid();
    Snowflake newChannel = leftVoice ? Snowflake::Invalid : state.channelId.get();

    // A user has one voice state, whichever of their sessions holds it. The account's own state
    // is this client's connection only while this gateway session holds it. Held by another
    // session, the account is a participant like any other and this client is not in voice,
    // which is how the official client treats it.
    bool isOwnConnection = isMe && (gatewaySessionId.isEmpty() || state.sessionId.get() == gatewaySessionId);

    bool takenByAnotherSession = isMe && !isOwnConnection && !leftVoice && channelId.isValid();
    Snowflake takenChannel = channelId;
    Snowflake takenGuild = guildId;
    if (takenByAnotherSession) {
        voiceSessionId.clear();
        channelId = Snowflake::Invalid;
        guildId = Snowflake::Invalid;
        selfStream = false;
        selfVideo = false;
        pending = {};
    }

    // cache every voice state that is not this client's own connection, so participants can be
    // populated when joining a channel
    if (!isOwnConnection) {
        Snowflake oldChannel;
        bool hadOld = false;
        Discord::VoiceState oldState;
        auto oldIt = knownVoiceStates.constFind(userId);
        if (oldIt != knownVoiceStates.constEnd()) {
            hadOld = true;
            oldState = oldIt.value();
            if (!oldIt->channelId.isNull())
                oldChannel = oldIt->channelId.get();
        }

        if (leftVoice)
            knownVoiceStates.remove(userId);
        else
            knownVoiceStates[userId] = state;

        if (oldChannel != newChannel) {
            if (oldChannel.isValid())
                emit channelVoiceMemberChanged(oldChannel, userId, false);
            if (newChannel.isValid())
                emit channelVoiceMemberChanged(newChannel, userId, true);
        } else if (newChannel.isValid() && hadOld) {
            bool stateDiffers = oldState.selfMute.get() != state.selfMute.get() ||
                                oldState.selfDeaf.get() != state.selfDeaf.get() ||
                                oldState.mute.get() != state.mute.get() ||
                                oldState.deaf.get() != state.deaf.get() ||
                                oldState.suppress.get() != state.suppress.get() ||
                                oldState.selfVideo.get() != state.selfVideo.get() ||
                                oldState.selfStream.valueOr(false) != state.selfStream.valueOr(false);
            if (stateDiffers)
                emit participantVoiceStateChanged(newChannel, userId);
        }
    }

    if (takenByAnotherSession) {
        qCInfo(LogVoice) << "Voice state: another session of this account connected, leaving voice here";

        if (takenChannel != newChannel)
            emit channelVoiceMemberChanged(takenChannel, accountId, false);

        if (!voiceConnection && !participants.isEmpty()) {
            participants.clear();
            emit participantsCleared();
        }
        disconnect();
        emit takenOverByAnotherSession(takenChannel, takenGuild);
        return;
    }

    if (isOwnConnection) {
        // until now the account's voice state may have been another session's
        auto otherSessionState = knownVoiceStates.constFind(accountId);
        if (otherSessionState != knownVoiceStates.constEnd()) {
            Snowflake otherSessionChannel = otherSessionState->channelId.isNull() ? Snowflake::Invalid : otherSessionState->channelId.get();
            knownVoiceStates.remove(accountId);
            if (otherSessionChannel.isValid() && otherSessionChannel != newChannel)
                emit channelVoiceMemberChanged(otherSessionChannel, accountId, false);
        }

        if (state.channelId.isNull() || !state.channelId.get().isValid()) {
            qCInfo(LogVoice) << "Voice state: disconnected from channel";
            Snowflake oldChannel = channelId;
            voiceSessionId.clear();
            channelId = Snowflake::Invalid;
            guildId = Snowflake::Invalid;
            selfStream = false;
            selfVideo = false;
            pending = {};

            if (oldChannel.isValid())
                emit channelVoiceMemberChanged(oldChannel, accountId, false);

            if (voiceConnection)
                disconnect();
            return;
        }

        Snowflake newChannelId = state.channelId.get();
        Snowflake oldSelfChannel = channelId;
        bool channelChanged = (channelId != newChannelId);

        voiceSessionId = state.sessionId.get();
        channelId = newChannelId;
        guildId = state.guildId.hasValue() ? state.guildId.get() : Snowflake::Invalid;

        // Before anything is announced: whoever hears of the new channel reads these too.
        bool wasMuted = selfMute;
        bool wasDeaf = selfDeaf;
        bool wasStreaming = selfStream;
        bool hadVideo = selfVideo;
        selfMute = state.selfMute.get();
        selfDeaf = state.selfDeaf.get();
        selfStream = state.selfStream.valueOr(false);
        selfVideo = state.selfVideo.get();

        if (channelChanged) {
            if (!participants.isEmpty()) {
                participants.clear();
                emit participantsCleared();
            }
            populateParticipantsFromCache();

            if (oldSelfChannel.isValid())
                emit channelVoiceMemberChanged(oldSelfChannel, accountId, false);
            emit channelVoiceMemberChanged(newChannelId, accountId, true);
        }

        qCInfo(LogVoice) << "Voice state: session =" << voiceSessionId
                         << "channel =" << channelId << "guild =" << guildId;

        if (!channelChanged && channelId.isValid() && (selfMute != wasMuted || selfDeaf != wasDeaf || selfStream != wasStreaming || selfVideo != hadVideo))
            emit participantVoiceStateChanged(channelId, accountId);

        if (audioPipeline && (selfMute != wasMuted || selfDeaf != wasDeaf)) {
            if (selfMute || selfDeaf)
                QMetaObject::invokeMethod(audioPipeline, &AudioPipeline::stopCapture);
            else
                QMetaObject::invokeMethod(audioPipeline, &AudioPipeline::startCapture);

            if (selfDeaf != wasDeaf)
                QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, deaf = selfDeaf]() { p->setDeafened(deaf); });
        }

        pending.hasStateUpdate = true;

        if (pending.hasServerUpdate && pending.guildId == guildId) {
            connectToVoiceServer(pending.endpoint, pending.token);
            pending = {};
        }
    }

    // others
    if (!channelId.isValid() || isMe)
        return;

    bool userLeftChannel = state.channelId.isNull() || !state.channelId.get().isValid() || state.channelId.get() != channelId;

    if (userLeftChannel) {
        if (participants.remove(userId)) {
            if (audioPipeline)
                QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, userId]() { p->removeUser(userId); });
            emit participantLeft(userId);
        }
    } else {
        auto it = participants.find(userId);
        bool isNew = (it == participants.end());

        VoiceParticipant &p = isNew ? participants[userId] : it.value();
        if (isNew)
            p.userId = userId;

        p.selfMute = state.selfMute.get();
        p.selfDeaf = state.selfDeaf.get();
        p.serverMute = state.mute.get();
        p.serverDeaf = state.deaf.get();
        p.suppress = state.suppress.get();
        p.selfVideo = state.selfVideo.get();
        p.selfStream = state.selfStream.valueOr(false);

        if (isNew)
            emit participantJoined(userId);
        else
            emit participantUpdated(userId);
    }
}

void VoiceManager::handleVoiceServerUpdate(const Discord::VoiceServerUpdate &event)
{
    Snowflake eventGuildId = event.guildId.get();

    if (event.endpoint.isNull() || event.endpoint.get().isEmpty()) {
        qCInfo(LogVoice) << "Voice server update with null endpoint, waiting for new one";
        return;
    }

    if (!proxy.canRelayUdp()) {
        qCWarning(LogVoice) << "Refusing voice connection: an HTTP proxy cannot relay voice UDP";
        return;
    }

    QString eventEndpoint = event.endpoint.get();
    QString eventToken = event.token.get();

    qCInfo(LogVoice) << "Voice server update: guild =" << eventGuildId
                     << "endpoint =" << eventEndpoint;

    if (voiceConnection && guildId == eventGuildId) {
        qCInfo(LogVoice) << "Server-commanded voice reconnection";
        stopVoiceThread();
        connectToVoiceServer(eventEndpoint, eventToken);
        return;
    }

    pending.token = eventToken;
    pending.endpoint = eventEndpoint;
    pending.guildId = eventGuildId;
    pending.hasServerUpdate = true;

    if (pending.hasStateUpdate && !voiceSessionId.isEmpty()) {
        connectToVoiceServer(eventEndpoint, eventToken);
        pending = {};
    }
}

void VoiceManager::setGatewaySessionId(const QString &sessionId)
{
    gatewaySessionId = sessionId;
}

void VoiceManager::forgetChannelVoiceStates(Snowflake targetChannelId)
{
    QList<Snowflake> gone;
    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        if (!it->channelId.isNull() && it->channelId.get() == targetChannelId)
            gone.append(it.key());
    }

    for (Snowflake userId : gone) {
        knownVoiceStates.remove(userId);
        emit channelVoiceMemberChanged(targetChannelId, userId, false);

        if (targetChannelId == channelId && participants.remove(userId)) {
            if (audioPipeline)
                QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, userId]() { p->removeUser(userId); });
            emit participantLeft(userId);
        }
    }
}

void VoiceManager::disconnect()
{
    stopVoiceThread();
    pending = {};
    emit voiceDisconnected();
    emit voiceStateChanged();
}

bool VoiceManager::isConnected() const
{
    return voiceClient &&
           voiceClient->state() == Discord::Voice::VoiceClient::State::Connected;
}

Discord::Voice::VoiceClient::State VoiceManager::clientState() const
{
    if (!voiceClient)
        return Discord::Voice::VoiceClient::State::Disconnected;
    return voiceClient->state();
}

void VoiceManager::setInputGain(float gain)
{
    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, gain]() { p->setInputGain(gain); });
}

void VoiceManager::setOutputVolume(float volume)
{
    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, volume]() { p->setOutputVolume(volume); });
}

void VoiceManager::setUserVolume(Snowflake userId, float volume)
{
    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, userId, volume]() { p->setUserVolume(userId, volume); });
}

void VoiceManager::setVadThreshold(float threshold)
{
    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, threshold]() { p->setVadThreshold(threshold); });
}

void VoiceManager::setOpusApplication(int application)
{
    cachedOpusApplication = application;

    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, application]() { p->setOpusApplication(application); });
}

void VoiceManager::setOpusBitrate(int bitrate)
{
    cachedOpusBitrate = bitrate;

    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, bitrate]() { p->setOpusBitrate(bitrate); });
}

void VoiceManager::setOpusComplexity(int complexity)
{
    cachedOpusComplexity = complexity;

    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, complexity]() { p->setOpusComplexity(complexity); });
}

void VoiceManager::setOpusSignalType(int signalType)
{
    cachedOpusSignalType = signalType;

    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, signalType]() { p->setOpusSignalType(signalType); });
}

void VoiceManager::setOpusFec(bool enabled)
{
    cachedOpusFec = enabled;

    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, enabled]() { p->setOpusFec(enabled); });
}

void VoiceManager::setOpusPacketLossPercent(int percent)
{
    cachedOpusPacketLossPercent = percent;

    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, percent]() { p->setOpusPacketLossPercent(percent); });
}

void VoiceManager::setNoiseSuppressionEnabled(bool enabled)
{
    cachedNoiseSuppression = enabled;

    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, enabled]() { p->setNoiseSuppressionEnabled(enabled); });
}

void VoiceManager::setUseRnnoiseVad(bool enabled)
{
    cachedUseRnnoiseVad = enabled;

    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, enabled]() { p->setUseRnnoiseVad(enabled); });
}

QList<AudioDeviceInfo> VoiceManager::availableInputDevices() const
{
    if (voiceConnection)
        return cachedInputDevices;
    return backend()->availableInputDevices();
}

QList<AudioDeviceInfo> VoiceManager::availableOutputDevices() const
{
    if (voiceConnection)
        return cachedOutputDevices;
    return backend()->availableOutputDevices();
}

void VoiceManager::setInputDevice(const QByteArray &deviceId)
{
    currentInputDeviceId = deviceId;
    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, deviceId]() { p->setInputDevice(deviceId); });
}

void VoiceManager::setOutputDevice(const QByteArray &deviceId)
{
    currentOutputDeviceId = deviceId;
    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, deviceId]() { p->setOutputDevice(deviceId); });
}

QList<VoiceParticipant> VoiceManager::currentParticipants() const
{
    return participants.values();
}

const VoiceParticipant *VoiceManager::participant(Snowflake userId) const
{
    auto it = participants.constFind(userId);
    if (it == participants.constEnd())
        return nullptr;
    return &it.value();
}

void VoiceManager::setUserMuted(Snowflake userId, bool muted)
{
    if (muted)
        mutedUsers.insert(userId);
    else
        mutedUsers.remove(userId);

    setUserVolume(userId, muted ? 0.0f : 1.0f);
}

bool VoiceManager::isUserMuted(Snowflake userId) const
{
    return mutedUsers.contains(userId);
}

int VoiceManager::channelVoiceUserCount(Snowflake targetChannelId) const
{
    if (!targetChannelId.isValid())
        return 0;

    int count = 0;
    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        if (!it->channelId.isNull() && it->channelId.get() == targetChannelId)
            ++count;
    }

    // include self in count
    if (channelId == targetChannelId && channelId.isValid())
        ++count;

    return count;
}

QList<Snowflake> VoiceManager::channelVoiceUsers(Snowflake targetChannelId) const
{
    QList<Snowflake> users;
    if (!targetChannelId.isValid())
        return users;

    if (channelId == targetChannelId && channelId.isValid())
        users.append(accountId);

    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        if (!it->channelId.isNull() && it->channelId.get() == targetChannelId)
            users.append(it.key());
    }

    return users;
}

std::optional<Discord::VoiceState> VoiceManager::voiceStateForUser(Snowflake userId) const
{
    if (userId == accountId && channelId.isValid()) {
        Discord::VoiceState state;
        state.userId = accountId;
        state.channelId = channelId;
        if (guildId.isValid())
            state.guildId = guildId;
        state.selfMute = selfMute;
        state.selfDeaf = selfDeaf;
        state.selfStream = selfStream;
        state.selfVideo = selfVideo;
        state.sessionId = voiceSessionId;
        return state;
    }

    auto it = knownVoiceStates.constFind(userId);
    if (it == knownVoiceStates.constEnd())
        return std::nullopt;
    return it.value();
}

bool VoiceManager::isDaveEnabled() const
{
    return voiceClient && voiceClient->isDaveEnabled();
}

void VoiceManager::requestVerificationCode(Snowflake targetUserId, std::function<void(const QString &)> callback)
{
    if (!voiceClient) {
        callback(QString());
        return;
    }
    QMetaObject::invokeMethod(voiceClient, [vc = voiceClient, targetUserId, cb = std::move(callback)]() mutable {
        vc->requestVerificationCode(targetUserId, std::move(cb));
    });
}

void VoiceManager::connectToVoiceServer(const QString &endpoint, const QString &token)
{
    if (voiceSessionId.isEmpty()) {
        qCWarning(LogVoice) << "Cannot connect to voice: no session ID";
        return;
    }

    stopVoiceThread();

    qCInfo(LogVoice) << "Creating voice thread for endpoint" << endpoint
                     << "guild" << guildId << "channel" << channelId;

    Discord::Voice::ConnectionParams params;
    params.endpoint = endpoint;
    params.token = token;
    params.serverId = guildId.isValid() ? guildId : channelId;
    params.channelId = channelId;
    params.userId = accountId;
    params.sessionId = voiceSessionId;
    params.proxy = proxy;
#ifdef ACHERON_HAVE_VIDEO
    // Declared whether or not the camera is ever turned on: without it the server does not
    // forward other people's video.
    params.video = true;
    params.streams = Discord::Voice::cameraStreams(Video::cameraSecondLayerEnabled());
    params.videoCodecs = Media::offeredVideoCodecs(encodableVideoCodecs);
#endif

    voiceConnection = new VoiceConnection(params, this);
    voiceClient = voiceConnection->client();
#ifdef ACHERON_HAVE_VIDEO
    callVideo->attach(voiceConnection, accountId);
#endif
    emit rtcConnectionCreated(voiceConnection);
    audioPipeline = new AudioPipeline;

#ifdef Q_OS_WIN
    // workaround for ma_context_init and ma_device_init being called on different threads
    // ma_context_init does the CoInitializeEx
    // will be fixed in miniaudio 0.12
    connect(voiceConnection->networkThread(), &QThread::started, this, [] { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }, Qt::DirectConnection);
    connect(voiceConnection->networkThread(), &QThread::finished, this, [] { CoUninitialize(); }, Qt::DirectConnection);
#endif

    QList<Snowflake> channelUsers;
    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        const auto &vs = it.value();
        if (!vs.channelId.isNull() && vs.channelId.get() == channelId)
            channelUsers.append(it.key());
    }
    voiceClient->seedConnectedUsers(channelUsers);

    cachedInputDevices = backend()->availableInputDevices();
    cachedOutputDevices = backend()->availableOutputDevices();

    voiceConnection->adopt(audioPipeline);
    voiceConnection->adopt(backend());

    connect(voiceClient, &Discord::Voice::VoiceClient::audioReceived, audioPipeline, &AudioPipeline::onAudioReceived);
    const QList<quint32> attachedSources = audioSources.keys();
    for (quint32 source : attachedSources)
        feedAudioSourceToPipeline(source);

    connect(audioPipeline, &AudioPipeline::encodedAudioReady, voiceClient, &Discord::Voice::VoiceClient::sendAudio);

    connect(audioPipeline, &AudioPipeline::speakingChanged, voiceClient, &Discord::Voice::VoiceClient::setSpeaking);

    connect(voiceClient, &Discord::Voice::VoiceClient::speakingReceived, audioPipeline,
            [ap = audioPipeline](const Discord::Voice::SpeakingData &data) {
                if (data.userId.hasValue() && data.userId->isValid() && data.ssrc.get() != 0)
                    ap->setSsrcUserId(data.ssrc, data.userId.get());
            });

    connect(voiceClient, &Discord::Voice::VoiceClient::clientConnected, audioPipeline,
            [ap = audioPipeline](const Discord::Voice::ClientConnectData &data) {
                if (data.userId.hasValue() && data.userId->isValid() && data.audioSsrc.get() != 0)
                    ap->setSsrcUserId(data.audioSsrc, data.userId.get());
            });

    // Who is in the call is who has a voice state in the channel, and besides them anyone the
    // voice server has connected whose voice state has not arrived (the official client's
    // RTCConnectionDesyncStore, 584569.js).
    connect(voiceConnection, &VoiceConnection::clientReachedServer,
            this, [this](Snowflake userId) {
                if (!userId.isValid() || userId == accountId)
                    return;
                populateParticipantsFromCache();
                if (participants.contains(userId))
                    return;
                VoiceParticipant p;
                p.userId = userId;
                participants.insert(userId, p);
                emit participantJoined(userId);
            });

    connect(voiceConnection, &VoiceConnection::clientDisconnected,
            this, [this](Snowflake userId) {
                if (audioPipeline)
                    QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, userId]() { p->removeUser(userId); });

                // Someone who moves to another of their devices drops off the voice server and
                // comes back, while their voice state stays in the channel throughout. Leaving
                // the server only takes away someone who was listed without a voice state.
                const auto voiceState = knownVoiceStates.constFind(userId);
                const bool stillInChannel = voiceState != knownVoiceStates.constEnd() && !voiceState->channelId.isNull() && voiceState->channelId.get() == channelId;
                auto participant = participants.find(userId);
                if (participant == participants.end())
                    return;
                if (!stillInChannel) {
                    participants.erase(participant);
                    emit participantLeft(userId);
                } else if (std::exchange(participant->speaking, false)) {
                    emit participantSpeakingChanged(userId, false);
                }
            });

    connect(voiceConnection, &VoiceConnection::speakingReceived,
            this, [this](const Discord::Voice::SpeakingData &data) {
                if (!data.userId.hasValue() || !data.userId->isValid())
                    return;
                Snowflake userId = data.userId.get();
                auto it = participants.find(userId);
                if (it == participants.end())
                    return;
                bool wasSpeaking = it->speaking;
                it->speaking = (data.speaking.get() != 0);
                if (it->speaking != wasSpeaking)
                    emit participantSpeakingChanged(userId, it->speaking);
            });

    connect(audioPipeline, &AudioPipeline::userAudioLevelChanged, this, &VoiceManager::userAudioLevelChanged);

    connect(voiceConnection, &VoiceConnection::connected, this, &VoiceManager::onVoiceClientConnected);
    connect(voiceConnection, &VoiceConnection::disconnected, this, &VoiceManager::onVoiceClientDisconnected);
    connect(voiceConnection, &VoiceConnection::stateChanged, this, &VoiceManager::onVoiceClientStateChanged);

    connect(audioPipeline, &AudioPipeline::audioLevelChanged, this, &VoiceManager::audioLevelChanged);

    connect(audioPipeline, &AudioPipeline::speakingChanged, this, &VoiceManager::speakingChanged);

    connect(voiceConnection, &VoiceConnection::privacyCodeChanged,
            this, [this](const QString &code) {
                cachedPrivacyCode = code;
                emit privacyCodeChanged(code);
            });

    if (!voiceConnection->start())
        stopVoiceThread();
}

void VoiceManager::attachAudioSource(quint32 source, Discord::Voice::VoiceClient *client)
{
    audioSources[source].client = client;
    feedAudioSourceToPipeline(source);
}

void VoiceManager::feedAudioSourceToPipeline(quint32 source)
{
    const AudioSource attached = audioSources.value(source);
    if (!audioPipeline || !attached.client)
        return;

    connect(attached.client, &Discord::Voice::VoiceClient::audioReceived, audioPipeline,
            [ap = audioPipeline, source](quint32 ssrc, uint16_t sequence, uint32_t timestamp, const QByteArray &opusData) {
                ap->onSourceAudioReceived(source, ssrc, sequence, timestamp, opusData);
            });
    QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, source, volume = attached.volume]() { p->setSourceVolume(source, volume); });
}

void VoiceManager::detachAudioSource(quint32 source)
{
    const AudioSource detached = audioSources.take(source);
    if (!audioPipeline)
        return;
    if (detached.client)
        QObject::disconnect(detached.client, &Discord::Voice::VoiceClient::audioReceived, audioPipeline, nullptr);
    QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, source]() { p->removeSource(source); });
}

void VoiceManager::setAudioSourceVolume(quint32 source, float volume)
{
    const auto attached = audioSources.find(source);
    if (attached != audioSources.end())
        attached->volume = volume;
    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, source, volume]() { p->setSourceVolume(source, volume); });
}

void VoiceManager::setEncodableVideoCodecs(const QList<Discord::Voice::VideoCodec> &codecs)
{
    if (codecs == encodableVideoCodecs)
        return;
    encodableVideoCodecs = codecs;
    if (voiceConnection)
        voiceConnection->run([codecs](Discord::Voice::VoiceClient *client) { client->offerVideoEncoding(codecs); });
}

void VoiceManager::stopVoiceThread()
{
    if (!voiceConnection)
        return;

    emit rtcConnectionClosing();
    bool hadParticipants = !participants.isEmpty();
    participants.clear();
    mutedUsers.clear();
    cachedPrivacyCode.clear();
    if (hadParticipants)
        emit participantsCleared();

    voiceConnection->stop([ap = audioPipeline]() { ap->stop(); });
#ifdef ACHERON_HAVE_VIDEO
    // Only now can no more frames arrive for its decoders.
    callVideo->detach();
#endif

    delete audioPipeline;
    audioPipeline = nullptr;

    // Not deleted here: this may be one of its own signals.
    voiceConnection->deleteLater();
    voiceConnection = nullptr;
    voiceClient = nullptr;

    qCDebug(LogVoice) << "Voice thread stopped";
}

void VoiceManager::populateParticipantsFromCache()
{
    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        const auto &vs = it.value();
        if (vs.channelId.isNull() || vs.channelId.get() != channelId)
            continue;
        if (participants.contains(it.key()))
            continue;

        VoiceParticipant p;
        p.userId = it.key();
        p.selfMute = vs.selfMute.get();
        p.selfDeaf = vs.selfDeaf.get();
        p.serverMute = vs.mute.get();
        p.serverDeaf = vs.deaf.get();
        p.suppress = vs.suppress.get();
        p.selfVideo = vs.selfVideo.get();
        p.selfStream = vs.selfStream.valueOr(false);
        participants.insert(it.key(), p);
        emit participantJoined(it.key());
    }
}

void VoiceManager::onVoiceClientConnected()
{
    if (!audioPipeline)
        return;

    qCInfo(LogVoice) << "Voice connection established for channel" << channelId;

    bool capturing = !selfMute && !selfDeaf;
    QByteArray inputId = currentInputDeviceId;
    QByteArray outputId = currentOutputDeviceId;
    IAudioBackend *audio = backend();
    int application = cachedOpusApplication;
    int bitrate = cachedOpusBitrate;
    int complexity = cachedOpusComplexity;
    int signalType = cachedOpusSignalType;
    bool fec = cachedOpusFec;
    int plp = cachedOpusPacketLossPercent;
    bool ns = cachedNoiseSuppression;
    bool nsVad = cachedUseRnnoiseVad;
    QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, audio, capturing, inputId, outputId,
                                              application, bitrate, complexity, signalType, fec, plp, ns, nsVad]() {
        p->setOpusApplication(application);
        p->setOpusBitrate(bitrate);
        p->setOpusComplexity(complexity);
        p->setOpusSignalType(signalType);
        p->setOpusFec(fec);
        p->setOpusPacketLossPercent(plp);
        p->setNoiseSuppressionEnabled(ns);
        p->setUseRnnoiseVad(nsVad);
        p->start(audio, capturing);
        if (!inputId.isEmpty())
            p->setInputDevice(inputId);
        if (!outputId.isEmpty())
            p->setOutputDevice(outputId);
    });

    populateParticipantsFromCache();

    emit voiceConnected();
    emit voiceStateChanged();
}

void VoiceManager::onVoiceClientDisconnected()
{
    qCInfo(LogVoice) << "Voice client disconnected";

    stopVoiceThread();

    emit voiceDisconnected();
    emit voiceStateChanged();
}

void VoiceManager::onVoiceClientStateChanged(Discord::Voice::VoiceClient::State state)
{
    qCDebug(LogVoice) << "VoiceManager: client state ->" << static_cast<int>(state);
    emit voiceStateChanged();
}

void VoiceManager::onDevicesChanged(const QList<AudioDeviceInfo> &inputs, const QList<AudioDeviceInfo> &outputs)
{
    cachedInputDevices = inputs;
    cachedOutputDevices = outputs;
    emit devicesChanged();
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
