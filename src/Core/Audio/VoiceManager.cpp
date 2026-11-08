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

namespace {

// the pipeline's thread
void applySettings(AudioPipeline *pipeline, const VoiceSettings &chosen)
{
    pipeline->setVadThreshold(static_cast<float>(chosen.vadThreshold));
    pipeline->setNoiseSuppressionEnabled(chosen.noiseSuppression);
    pipeline->setUseRnnoiseVad(chosen.rnnoiseVad);
    pipeline->setOpusApplication(chosen.opusApplication);
    pipeline->setOpusBitrate(chosen.opusBitrateKbps * 1000);
    pipeline->setOpusComplexity(chosen.opusComplexity);
    pipeline->setOpusSignalType(chosen.opusSignalType);
    pipeline->setOpusFec(chosen.opusFec);
    pipeline->setOpusPacketLossPercent(chosen.opusPacketLossPercent);
    pipeline->setInputGain(chosen.inputGain);
    pipeline->setOutputVolume(chosen.outputVolume);
    pipeline->setInputDevice(chosen.inputDevice);
    pipeline->setOutputDevice(chosen.outputDevice);
}

} // namespace

VoiceManager::VoiceManager(Snowflake accountId, const ProxyConfig &proxy, QObject *parent)
    : QObject(parent), accountId(accountId), proxy(proxy)
{
#ifdef ACHERON_HAVE_VIDEO
    callVideo = new CallVideo(this);
#endif
    settings = VoiceSettings::load();
    userVolumes = savedUserVolumes();
    mutedUsers = savedUserMutes();
    connect(VoiceSettingsNotifier::instance(), &VoiceSettingsNotifier::changed, this, [this] { adopt(VoiceSettings::load()); });
}

VoiceManager::~VoiceManager()
{
    closeVoiceConnection();
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
    const Snowflake newChannel = state.channel();
    const bool leftVoice = !newChannel.isValid();

    // as the official client: the account's state is this connection only while this gateway session holds it
    bool isOwnConnection = isMe && state.isHeldBy(gatewaySessionId);

    bool takenByAnotherSession = isMe && !isOwnConnection && !leftVoice && channelId.isValid();
    Snowflake takenChannel = channelId;
    Snowflake takenGuild = guildId;
    if (takenByAnotherSession)
        forgetOwnVoiceState();

    if (!isOwnConnection) {
        Snowflake oldChannel;
        bool hadOld = false;
        Discord::VoiceState oldState;
        auto oldIt = knownVoiceStates.constFind(userId);
        if (oldIt != knownVoiceStates.constEnd()) {
            hadOld = true;
            oldState = oldIt.value();
            oldChannel = oldState.channel();
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
            if (VoiceParticipant::of(userId, oldState) != VoiceParticipant::of(userId, state))
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
        auto otherSessionState = knownVoiceStates.constFind(accountId);
        if (otherSessionState != knownVoiceStates.constEnd()) {
            const Snowflake otherSessionChannel = otherSessionState->channel();
            knownVoiceStates.remove(accountId);
            if (otherSessionChannel.isValid() && otherSessionChannel != newChannel)
                emit channelVoiceMemberChanged(otherSessionChannel, accountId, false);
        }

        if (leftVoice) {
            qCInfo(LogVoice) << "Voice state: disconnected from channel";
            Snowflake oldChannel = channelId;
            forgetOwnVoiceState();

            if (oldChannel.isValid())
                emit channelVoiceMemberChanged(oldChannel, accountId, false);

            if (voiceConnection)
                disconnect();
            return;
        }

        Snowflake oldSelfChannel = channelId;
        bool channelChanged = (channelId != newChannel);

        voiceSessionId = state.sessionId.get();
        channelId = newChannel;
        guildId = state.guildId.hasValue() ? state.guildId.get() : Snowflake::Invalid;

        // self_mute and self_deaf stay as this client last chose them, which an echo can trail
        bool wasStreaming = selfStream;
        bool hadVideo = selfVideo;
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
            emit channelVoiceMemberChanged(newChannel, accountId, true);
        }

        qCInfo(LogVoice) << "Voice state: session =" << voiceSessionId
                         << "channel =" << channelId << "guild =" << guildId;

        if (!channelChanged && channelId.isValid() && (selfStream != wasStreaming || selfVideo != hadVideo))
            emit participantVoiceStateChanged(channelId, accountId);

        pending.hasStateUpdate = true;

        if (pending.hasServerUpdate && pending.guildId == guildId) {
            connectToVoiceServer(pending.endpoint, pending.token);
            pending = {};
        }
    }

    // others
    if (!channelId.isValid() || isMe)
        return;

    if (newChannel != channelId) {
        if (participants.remove(userId)) {
            dropUserAudio(userId);
            emit participantLeft(userId);
        }
    } else {
        auto it = participants.find(userId);
        const bool isNew = it == participants.end();

        VoiceParticipant updated = VoiceParticipant::of(userId, state);
        if (!isNew)
            updated.speaking = it->speaking;
        participants.insert(userId, updated);

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
        closeVoiceConnection();
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

void VoiceManager::forgetOwnVoiceState()
{
    voiceSessionId.clear();
    channelId = Snowflake::Invalid;
    guildId = Snowflake::Invalid;
    selfStream = false;
    selfVideo = false;
    pending = {};
}

void VoiceManager::toggleSelfMute()
{
    if (selfDeaf)
        setSelfAudio(false, false);
    else
        setSelfAudio(!selfMute, false);
    emit selfAudioToggled();
}

void VoiceManager::toggleSelfDeafen()
{
    if (selfDeaf) {
        setSelfAudio(mutedBeforeDeafening, false);
    } else {
        mutedBeforeDeafening = selfMute;
        setSelfAudio(true, true);
    }
    emit selfAudioToggled();
}

void VoiceManager::resetSelfAudio()
{
    setSelfAudio(false, false);
}

void VoiceManager::setSelfAudio(bool muted, bool deafened)
{
    const bool wasMuted = std::exchange(selfMute, muted);
    const bool wasDeaf = std::exchange(selfDeaf, deafened);
    if (selfMute == wasMuted && selfDeaf == wasDeaf)
        return;

    if (audioPipeline) {
        if (selfMute || selfDeaf)
            QMetaObject::invokeMethod(audioPipeline, &AudioPipeline::stopCapture);
        else
            QMetaObject::invokeMethod(audioPipeline, &AudioPipeline::startCapture);
        if (selfDeaf != wasDeaf)
            QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, deaf = selfDeaf]() { p->setDeafened(deaf); });
    }
    if (channelId.isValid())
        emit participantVoiceStateChanged(channelId, accountId);
    emit selfAudioChanged();
}

void VoiceManager::setGatewaySessionId(const QString &sessionId)
{
    gatewaySessionId = sessionId;
}

void VoiceManager::forgetChannelVoiceStates(Snowflake targetChannelId)
{
    QList<Snowflake> gone;
    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        if (it->channel() == targetChannelId)
            gone.append(it.key());
    }

    for (Snowflake userId : gone) {
        knownVoiceStates.remove(userId);
        emit channelVoiceMemberChanged(targetChannelId, userId, false);

        if (targetChannelId == channelId && participants.remove(userId)) {
            dropUserAudio(userId);
            emit participantLeft(userId);
        }
    }
}

void VoiceManager::disconnect()
{
    closeVoiceConnection();
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

void VoiceManager::adopt(const VoiceSettings &chosen)
{
    settings = chosen;
    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, chosen]() { applySettings(p, chosen); });
}

void VoiceManager::setUserVolume(Snowflake userId, float gain)
{
    if (gain == 1.0f)
        userVolumes.remove(userId);
    else
        userVolumes.insert(userId, gain);
    saveUserVolume(userId, gain);
    applyUserVolume(userId);
    emit userPlaybackChanged(userId);
}

float VoiceManager::userVolume(Snowflake userId) const
{
    const auto found = userVolumes.constFind(userId);
    return found == userVolumes.constEnd() ? 1.0f : found.value();
}

float VoiceManager::effectiveVolumeOf(Snowflake userId) const
{
    return mutedUsers.contains(userId) ? 0.0f : userVolume(userId);
}

void VoiceManager::applyUserVolume(Snowflake userId)
{
    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, userId, volume = effectiveVolumeOf(userId)]() { p->setUserVolume(userId, volume); });
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
    saveUserMuted(userId, muted);
    applyUserVolume(userId);
    emit userPlaybackChanged(userId);
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
        if (it->channel() == targetChannelId)
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
        if (it->channel() == targetChannelId)
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

    closeVoiceConnection();

    qCInfo(LogVoice) << "Opening voice connection to endpoint" << endpoint
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
    // without it the server forwards no one's video
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
        if (it->channel() == channelId)
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

    connect(voiceClient, &Discord::Voice::VoiceClient::videoStateReceived, audioPipeline,
            [ap = audioPipeline](const Discord::Voice::RemoteVideoState &data) {
                if (data.userId.hasValue() && data.userId->isValid() && data.audioSsrc.get() != 0)
                    ap->setSsrcUserId(data.audioSsrc, data.userId.get());
            });

    // as the official client's RTCConnectionDesyncStore: also anyone the voice server connected whose voice state hasnt arrived
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
                dropUserAudio(userId);

                // someone moving to another device drops off the voice server and comes back while their voice state stays
                const auto voiceState = knownVoiceStates.constFind(userId);
                const bool stillInChannel = voiceState != knownVoiceStates.constEnd() && voiceState->channel() == channelId;
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

    connect(audioPipeline, &AudioPipeline::userSpeakingChanged, this, [this](Snowflake userId, bool speaking) {
        auto it = participants.find(userId);
        if (it == participants.end() || std::exchange(it->speaking, speaking) == speaking)
            return;
        emit participantSpeakingChanged(userId, speaking);
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
        closeVoiceConnection();
}

quint32 VoiceManager::newAudioSource()
{
    return AudioPipeline::VOICE_CONNECTION_SOURCE + ++addedAudioSources;
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
            [ap = audioPipeline, source](quint32 ssrc, uint16_t sequence, uint32_t timestamp, const QByteArray &opusData, std::optional<int>) {
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

void VoiceManager::dropUserAudio(Snowflake userId)
{
    if (audioPipeline)
        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, userId]() { p->removeUser(userId); });
}

void VoiceManager::closeVoiceConnection()
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
    callVideo->detach();
#endif

    delete audioPipeline;
    audioPipeline = nullptr;

    // not deleted here: this may be one of its own signals
    voiceConnection->deleteLater();
    voiceConnection = nullptr;
    voiceClient = nullptr;

    qCDebug(LogVoice) << "Voice connection closed";
}

void VoiceManager::populateParticipantsFromCache()
{
    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        if (it->channel() != channelId || participants.contains(it.key()))
            continue;

        participants.insert(it.key(), VoiceParticipant::of(it.key(), it.value()));
        emit participantJoined(it.key());
    }
}

void VoiceManager::onVoiceClientConnected()
{
    if (!audioPipeline)
        return;

    qCInfo(LogVoice) << "Voice connection established for channel" << channelId;

    QHash<Snowflake, float> volumes;
    for (auto it = userVolumes.constBegin(); it != userVolumes.constEnd(); ++it)
        volumes.insert(it.key(), effectiveVolumeOf(it.key()));
    for (const Snowflake muted : std::as_const(mutedUsers))
        volumes.insert(muted, 0.0f);
    QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, audio = backend(), capturing = !selfMute && !selfDeaf, deafened = selfDeaf, chosen = settings, volumes]() {
        p->start(audio, capturing);
        applySettings(p, chosen);
        p->setDeafened(deafened);
        for (auto it = volumes.constBegin(); it != volumes.constEnd(); ++it)
            p->setUserVolume(it.key(), it.value());
    });

    populateParticipantsFromCache();

    emit voiceConnected();
    emit voiceStateChanged();
}

void VoiceManager::onVoiceClientDisconnected()
{
    qCInfo(LogVoice) << "Voice client disconnected";

    closeVoiceConnection();

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
