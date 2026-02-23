#include "VoiceManager.hpp"
#include "AudioPipeline.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Core {
namespace AV {

VoiceManager::VoiceManager(Snowflake accountId, QObject *parent)
    : QObject(parent), accountId(accountId), audioBackend(IAudioBackend::create())
{
    connect(audioBackend.get(), &IAudioBackend::devicesChanged, this, &VoiceManager::onDevicesChanged);
}

VoiceManager::~VoiceManager()
{
    stopVoiceThread();
}

void VoiceManager::handleVoiceStateUpdate(const Discord::VoiceState &state)
{
    Snowflake userId = state.userId.get();
    bool isMe = (userId == accountId);

    // cache all non-self voice states so we can populate participants when joining a channel
    if (!isMe) {
        bool leftVoice = state.channelId.isNull() || !state.channelId.get().isValid();
        if (leftVoice)
            knownVoiceStates.remove(userId);
        else
            knownVoiceStates[userId] = state;
    }

    if (isMe) {
        if (state.channelId.isNull() || !state.channelId.get().isValid()) {
            qCInfo(LogVoice) << "Voice state: disconnected from channel";
            voiceSessionId.clear();
            channelId = Snowflake::Invalid;
            guildId = Snowflake::Invalid;
            pending = {};

            if (voiceThread)
                disconnect();
            return;
        }

        Snowflake newChannelId = state.channelId.get();
        bool channelChanged = (channelId != newChannelId);

        voiceSessionId = state.sessionId.get();
        channelId = newChannelId;
        guildId = state.guildId.hasValue() ? state.guildId.get() : Snowflake::Invalid;

        if (channelChanged) {
            if (!participants.isEmpty()) {
                participants.clear();
                emit participantsCleared();
            }
            populateParticipantsFromCache();
        }

        bool wasMuted = selfMute;
        bool wasDeaf = selfDeaf;
        selfMute = state.selfMute.get();
        selfDeaf = state.selfDeaf.get();

        qCInfo(LogVoice) << "Voice state: session =" << voiceSessionId
                         << "channel =" << channelId << "guild =" << guildId;

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

    QString eventEndpoint = event.endpoint.get();
    QString eventToken = event.token.get();

    qCInfo(LogVoice) << "Voice server update: guild =" << eventGuildId
                     << "endpoint =" << eventEndpoint;

    if (voiceThread && guildId == eventGuildId) {
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
           voiceClient->state() == Discord::AV::VoiceClient::State::Connected;
}

Discord::AV::VoiceClient::State VoiceManager::clientState() const
{
    if (!voiceClient)
        return Discord::AV::VoiceClient::State::Disconnected;
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

QList<AudioDeviceInfo> VoiceManager::availableInputDevices() const
{
    if (voiceThread)
        return cachedInputDevices;
    return audioBackend->availableInputDevices();
}

QList<AudioDeviceInfo> VoiceManager::availableOutputDevices() const
{
    if (voiceThread)
        return cachedOutputDevices;
    return audioBackend->availableOutputDevices();
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

    voiceThread = new QThread(this);
    voiceThread->setObjectName("VoiceThread");

    voiceClient = new Discord::AV::VoiceClient(endpoint, token, guildId, channelId, accountId, voiceSessionId);
    audioPipeline = new AudioPipeline;

    QList<Snowflake> channelUsers;
    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        const auto &vs = it.value();
        if (!vs.channelId.isNull() && vs.channelId.get() == channelId)
            channelUsers.append(it.key());
    }
    voiceClient->seedConnectedUsers(channelUsers);

    cachedInputDevices = audioBackend->availableInputDevices();
    cachedOutputDevices = audioBackend->availableOutputDevices();

    voiceClient->moveToThread(voiceThread);
    audioPipeline->moveToThread(voiceThread);
    audioBackend->moveToThread(voiceThread);

    connect(voiceClient, &Discord::AV::VoiceClient::audioReceived, audioPipeline, &AudioPipeline::onAudioReceived);

    connect(audioPipeline, &AudioPipeline::encodedAudioReady, voiceClient, &Discord::AV::VoiceClient::sendAudio);

    connect(audioPipeline, &AudioPipeline::speakingChanged, voiceClient, &Discord::AV::VoiceClient::setSpeaking);

    connect(voiceClient, &Discord::AV::VoiceClient::speakingReceived, audioPipeline,
            [ap = audioPipeline](const Discord::AV::SpeakingData &data) {
                if (data.userId.hasValue() && data.userId->isValid() && data.ssrc.get() != 0)
                    ap->setSsrcUserId(data.ssrc, data.userId.get());
            });

    connect(voiceClient, &Discord::AV::VoiceClient::clientConnected, audioPipeline,
            [ap = audioPipeline](const Discord::AV::ClientConnectData &data) {
                if (data.userId.hasValue() && data.userId->isValid() && data.audioSsrc.get() != 0)
                    ap->setSsrcUserId(data.audioSsrc, data.userId.get());
            });

    // guard new connections (against eg channel moves)
    unsigned int gen = voiceGeneration;

    connect(voiceClient, &Discord::AV::VoiceClient::clientConnected,
            this, [this, gen](const Discord::AV::ClientConnectData &data) {
                if (gen != voiceGeneration)
                    return;
                Snowflake userId = data.userId;
                if (!userId.isValid() || userId == accountId || participants.contains(userId))
                    return;
                VoiceParticipant p;
                p.userId = userId;
                participants.insert(userId, p);
                emit participantJoined(userId);
            });

    connect(voiceClient, &Discord::AV::VoiceClient::clientDisconnected,
            this, [this, gen](Snowflake userId) {
                if (gen != voiceGeneration)
                    return;
                if (participants.remove(userId)) {
                    if (audioPipeline)
                        QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, userId]() { p->removeUser(userId); });
                    emit participantLeft(userId);
                }
            });

    connect(voiceClient, &Discord::AV::VoiceClient::speakingReceived,
            this, [this, gen](const Discord::AV::SpeakingData &data) {
                if (gen != voiceGeneration)
                    return;
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

    connect(voiceClient, &Discord::AV::VoiceClient::connected,
            this, [this, gen]() {
                if (gen != voiceGeneration)
                    return;
                onVoiceClientConnected();
            });

    connect(voiceClient, &Discord::AV::VoiceClient::disconnected, this, [this, gen]() {
                if (gen != voiceGeneration)
                    return;
                onVoiceClientDisconnected(); }, Qt::QueuedConnection);

    connect(voiceClient, &Discord::AV::VoiceClient::stateChanged,
            this, [this, gen](Discord::AV::VoiceClient::State state) {
                if (gen != voiceGeneration)
                    return;
                onVoiceClientStateChanged(state);
            });

    connect(audioPipeline, &AudioPipeline::audioLevelChanged, this, &VoiceManager::audioLevelChanged);

    connect(audioPipeline, &AudioPipeline::speakingChanged, this, &VoiceManager::speakingChanged);

    connect(voiceClient, &Discord::AV::VoiceClient::privacyCodeChanged,
            this, [this, gen](const QString &code) {
                if (gen != voiceGeneration)
                    return;
                cachedPrivacyCode = code;
                emit privacyCodeChanged(code);
            });

    voiceThread->start();
    QMetaObject::invokeMethod(voiceClient, &Discord::AV::VoiceClient::start);
}

void VoiceManager::stopVoiceThread()
{
    if (!voiceThread)
        return;

    voiceGeneration++;
    bool hadParticipants = !participants.isEmpty();
    participants.clear();
    mutedUsers.clear();
    cachedPrivacyCode.clear();
    if (hadParticipants)
        emit participantsCleared();

    AudioPipeline *ap = audioPipeline;
    Discord::AV::VoiceClient *vc = voiceClient;
    IAudioBackend *backend = audioBackend.get();
    QThread *mainThread = thread();
    QMetaObject::invokeMethod(audioPipeline, [ap, vc, backend, mainThread]() {
        ap->stop();
        vc->stop();
        ap->moveToThread(mainThread);
        vc->moveToThread(mainThread);
        backend->moveToThread(mainThread); }, Qt::BlockingQueuedConnection);

    voiceThread->quit();
    if (!voiceThread->wait(5000)) {
        qCWarning(LogVoice) << "Voice thread did not stop in time, terminating";
        voiceThread->terminate();
        voiceThread->wait();
    }

    delete audioPipeline;
    audioPipeline = nullptr;

    delete voiceClient;
    voiceClient = nullptr;

    delete voiceThread;
    voiceThread = nullptr;

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
    IAudioBackend *backend = audioBackend.get();
    QMetaObject::invokeMethod(audioPipeline, [p = audioPipeline, backend, capturing, inputId, outputId]() {
        p->start(backend, capturing);
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

void VoiceManager::onVoiceClientStateChanged(Discord::AV::VoiceClient::State state)
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

} // namespace AV
} // namespace Core
} // namespace Acheron
