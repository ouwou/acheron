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
    if (state.userId.get() != accountId)
        return;

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

    voiceSessionId = state.sessionId.get();
    channelId = state.channelId.get();
    guildId = state.guildId.hasValue() ? state.guildId.get() : Snowflake::Invalid;

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

    cachedInputDevices = audioBackend->availableInputDevices();
    cachedOutputDevices = audioBackend->availableOutputDevices();

    voiceClient->moveToThread(voiceThread);
    audioPipeline->moveToThread(voiceThread);
    audioBackend->moveToThread(voiceThread);

    voiceConnections.append(
            connect(voiceClient, &Discord::AV::VoiceClient::audioReceived,
                    audioPipeline, &AudioPipeline::onAudioReceived));

    voiceConnections.append(
            connect(audioPipeline, &AudioPipeline::encodedAudioReady,
                    voiceClient, &Discord::AV::VoiceClient::sendAudio));

    voiceConnections.append(
            connect(audioPipeline, &AudioPipeline::speakingChanged,
                    voiceClient, &Discord::AV::VoiceClient::setSpeaking));

    voiceConnections.append(
            connect(voiceClient, &Discord::AV::VoiceClient::speakingReceived, audioPipeline,
                    [ap = audioPipeline](const Discord::AV::SpeakingData &data) {
                        if (data.userId->isValid() && data.ssrc.get() != 0)
                            ap->setSsrcUserId(data.ssrc, data.userId.get());
                    }));

    voiceConnections.append(
            connect(voiceClient, &Discord::AV::VoiceClient::clientConnected, audioPipeline,
                    [ap = audioPipeline](const Discord::AV::ClientConnectData &data) {
                        if (data.userId->isValid() && data.audioSsrc.get() != 0)
                            ap->setSsrcUserId(data.audioSsrc, data.userId);
                    }));

    // move to a new generation so old signals are ignored.
    // for example if we get dragged to a different channel a disconnect can sneak in
    unsigned int gen = voiceGeneration;

    voiceConnections.append(
            connect(voiceClient, &Discord::AV::VoiceClient::connected,
                    this, [this, gen]() {
                        if (gen != voiceGeneration)
                            return;
                        onVoiceClientConnected();
                    }));

    voiceConnections.append(
            connect(voiceClient, &Discord::AV::VoiceClient::disconnected, this, [this, gen]() {
                        if (gen != voiceGeneration)
                            return;
                        onVoiceClientDisconnected(); }, Qt::QueuedConnection));

    voiceConnections.append(
            connect(voiceClient, &Discord::AV::VoiceClient::stateChanged,
                    this, [this, gen](Discord::AV::VoiceClient::State state) {
                        if (gen != voiceGeneration)
                            return;
                        onVoiceClientStateChanged(state);
                    }));

    voiceConnections.append(
            connect(audioPipeline, &AudioPipeline::audioLevelChanged,
                    this, &VoiceManager::audioLevelChanged));

    voiceConnections.append(
            connect(audioPipeline, &AudioPipeline::speakingChanged,
                    this, &VoiceManager::speakingChanged));

    voiceThread->start();
    QMetaObject::invokeMethod(voiceClient, &Discord::AV::VoiceClient::start);
}

void VoiceManager::stopVoiceThread()
{
    if (!voiceThread)
        return;

    voiceGeneration++;

    for (auto &conn : voiceConnections)
        QObject::disconnect(conn);
    voiceConnections.clear();

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
        if (!inputId.isEmpty())
            p->setInputDevice(inputId);
        if (!outputId.isEmpty())
            p->setOutputDevice(outputId);
        p->start(backend, capturing);
    });

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
