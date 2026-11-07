#include "AudioPipeline.hpp"
#include "IAudioBackend.hpp"
#include "OpusEncoder.hpp"
#include "OpusDecoder.hpp"
#include "JitterBuffer.hpp"
#include "AudioMixer.hpp"
#include "NoiseSuppressor.hpp"

#include "Core/Logging.hpp"
#include "Discord/Voice/VoiceEnums.hpp"

#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Acheron {
namespace Core {
namespace Audio {

namespace {

quint64 speakerKey(quint32 source, quint32 ssrc)
{
    return (quint64(source) << 32) | ssrc;
}

quint32 speakerSource(quint64 key)
{
    return static_cast<quint32>(key >> 32);
}

quint32 speakerSsrc(quint64 key)
{
    return static_cast<quint32>(key);
}

} // namespace

AudioPipeline::AudioPipeline(QObject *parent)
    : QObject(parent)
{
}

AudioPipeline::~AudioPipeline()
{
}

void AudioPipeline::start(IAudioBackend *backend, bool capturing)
{
    if (audioBackend)
        return;

    audioBackend = backend;
    connect(audioBackend, &IAudioBackend::audioCaptured, this, &AudioPipeline::onAudioCaptured, Qt::QueuedConnection);

    initializeEncoder();

    noiseSuppressor = std::make_unique<NoiseSuppressor>();
    if (!noiseSuppressor->init())
        noiseSuppressor.reset();

    rmsThrottleTimer.start();
    userRmsThrottleTimer.start();
    speakingClock.start();

    if (capturing)
        audioBackend->startCapture();
    reconfigureNoiseSuppressorChannels();

    audioBackend->startPlayback();

    mixTimer = new QTimer(this);
    mixTimer->setTimerType(Qt::PreciseTimer);
    mixTimer->setInterval(AUDIO_FRAME_DURATION_MS);
    connect(mixTimer, &QTimer::timeout, this, &AudioPipeline::onMixTick);
    mixTimer->start();

    qCInfo(LogVoice) << "Audio pipeline started";
}

void AudioPipeline::stop()
{
    delete mixTimer;
    mixTimer = nullptr;

    if (audioBackend) {
        disconnect(audioBackend, &IAudioBackend::audioCaptured, this, &AudioPipeline::onAudioCaptured);
        audioBackend->stopCapture();
        audioBackend->stopPlayback();
    }

    speakers.clear();
    ssrcToUser.clear();
    pendingUserRms.clear();
    for (const Snowflake userId : lastVoiceMs.keys())
        stopSpeaking(userId);

    encoder.reset();
    noiseSuppressor.reset();

    audioBackend = nullptr;

    isSpeaking = false;
    vadHoldoffCounter = 0;

    qCDebug(LogVoice) << "Audio pipeline stopped";
}

void AudioPipeline::startCapture()
{
    if (!audioBackend)
        return;

    audioBackend->startCapture();
    reconfigureNoiseSuppressorChannels();
}

void AudioPipeline::stopCapture()
{
    if (!audioBackend)
        return;

    audioBackend->stopCapture();

    if (isSpeaking) {
        sendTrailingSilence();
        isSpeaking = false;
        vadHoldoffCounter = 0;
        emit speakingChanged(false);
    }
}

void AudioPipeline::onAudioReceived(quint32 ssrc, uint16_t sequence, uint32_t timestamp, const QByteArray &opusData, std::optional<int> speakingFlags)
{
    onSourceAudioReceived(VOICE_CONNECTION_SOURCE, ssrc, sequence, timestamp, opusData);
    noteSpeakingOf(ssrc, opusData, speakingFlags);
}

void AudioPipeline::setSourceVolume(quint32 source, float volume)
{
    sourceVolumes.insert(source, volume);
}

void AudioPipeline::removeSource(quint32 source)
{
    sourceVolumes.remove(source);
    std::erase_if(speakers, [source](const auto &entry) { return speakerSource(entry.first) == source; });
}

void AudioPipeline::onSourceAudioReceived(quint32 source, quint32 ssrc, uint16_t sequence, uint32_t /*timestamp*/, const QByteArray &opusData)
{
    const quint64 key = speakerKey(source, ssrc);
    auto it = speakers.find(key);
    if (it == speakers.end()) {
        SpeakerState state;
        state.decoder = std::make_unique<OpusDecoder>();
        if (!state.decoder->init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS)) {
            qCWarning(LogVoice) << "Failed to init decoder for SSRC" << ssrc;
            return;
        }
        state.jitterBuffer = std::make_unique<JitterBuffer>();

        auto [inserted, _] = speakers.emplace(key, std::move(state));
        it = inserted;
    }

    it->second.jitterBuffer->push(sequence, opusData);
}

// like the official media engine: the speaking flags each audio packet carries, not those of opcode 5, which only maps the ssrc
void AudioPipeline::noteSpeakingOf(quint32 ssrc, const QByteArray &opusData, std::optional<int> speakingFlags)
{
    const auto user = ssrcToUser.constFind(ssrc);
    if (user == ssrcToUser.constEnd())
        return;
    const bool talking = speakingFlags ? (*speakingFlags & static_cast<int>(Discord::Voice::SpeakingFlag::MICROPHONE)) != 0 : !isOpusSilence(opusData);
    if (!talking) {
        stopSpeaking(user.value());
        return;
    }
    const bool wasSpeaking = lastVoiceMs.contains(user.value());
    lastVoiceMs.insert(user.value(), speakingClock.elapsed());
    if (!wasSpeaking)
        emit userSpeakingChanged(user.value(), true);
}

void AudioPipeline::expireRemoteSpeaking()
{
    const qint64 now = speakingClock.elapsed();
    QList<Snowflake> quiet;
    for (auto it = lastVoiceMs.constBegin(); it != lastVoiceMs.constEnd(); ++it) {
        if (now - it.value() > REMOTE_SPEAKING_HOLD_MS)
            quiet.append(it.key());
    }
    for (const Snowflake userId : std::as_const(quiet))
        stopSpeaking(userId);
}

void AudioPipeline::stopSpeaking(Snowflake userId)
{
    if (lastVoiceMs.remove(userId))
        emit userSpeakingChanged(userId, false);
}

void AudioPipeline::setDeafened(bool deafened)
{
    this->deafened = deafened;
}

void AudioPipeline::setSsrcUserId(quint32 ssrc, Snowflake userId)
{
    ssrcToUser[ssrc] = userId;
}

void AudioPipeline::removeUser(Snowflake userId)
{
    for (auto it = ssrcToUser.begin(); it != ssrcToUser.end();) {
        if (it.value() == userId) {
            speakers.erase(speakerKey(VOICE_CONNECTION_SOURCE, it.key()));
            it = ssrcToUser.erase(it);
        } else {
            ++it;
        }
    }
    pendingUserRms.remove(userId);
    stopSpeaking(userId);
}

void AudioPipeline::setUserVolume(Snowflake userId, float volume)
{
    if (volume == 1.0f)
        userVolumes.remove(userId);
    else
        userVolumes.insert(userId, volume);
}

void AudioPipeline::setInputDevice(const QByteArray &deviceId)
{
    if (!audioBackend)
        return;

    audioBackend->setInputDevice(deviceId);
    reconfigureNoiseSuppressorChannels();
}

void AudioPipeline::reconfigureNoiseSuppressorChannels()
{
    if (noiseSuppressor && audioBackend && audioBackend->isCapturing())
        noiseSuppressor->reconfigure(audioBackend->nativeCaptureChannels());
}

void AudioPipeline::setOutputDevice(const QByteArray &deviceId)
{
    if (audioBackend)
        audioBackend->setOutputDevice(deviceId);
}

void AudioPipeline::setInputGain(float gain)
{
    if (audioBackend)
        audioBackend->setInputGain(gain);
}

void AudioPipeline::setOutputVolume(float volume)
{
    if (audioBackend)
        audioBackend->setOutputVolume(volume);
}

void AudioPipeline::setVadThreshold(float threshold)
{
    vadThreshold = threshold;
}

void AudioPipeline::setNoiseSuppressionEnabled(bool enabled)
{
    noiseSuppressionEnabled = enabled;
}

void AudioPipeline::setUseRnnoiseVad(bool enabled)
{
    useRnnoiseVad = enabled;
}

void AudioPipeline::initializeEncoder()
{
    encoder = std::make_unique<OpusEncoder>();
    if (!encoder->init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, opusApplication)) {
        qCCritical(LogVoice) << "Failed to initialize Opus encoder";
        encoder.reset();
        return;
    }
    encoder->setBitrate(opusBitrate);
    encoder->setComplexity(opusComplexity);
    encoder->setSignalType(opusSignalType);
    encoder->setFec(opusFec);
    encoder->setPacketLossPercent(opusPacketLossPercent);
}

void AudioPipeline::setOpusApplication(int application)
{
    if (opusApplication == application)
        return;

    opusApplication = application;

    if (encoder)
        initializeEncoder();
}

void AudioPipeline::setOpusBitrate(int bitrate)
{
    opusBitrate = bitrate;

    if (encoder)
        encoder->setBitrate(bitrate);
}

void AudioPipeline::setOpusComplexity(int complexity)
{
    opusComplexity = complexity;

    if (encoder)
        encoder->setComplexity(complexity);
}

void AudioPipeline::setOpusSignalType(int signalType)
{
    opusSignalType = signalType;

    if (encoder)
        encoder->setSignalType(signalType);
}

void AudioPipeline::setOpusFec(bool enabled)
{
    opusFec = enabled;

    if (encoder)
        encoder->setFec(enabled);
}

void AudioPipeline::setOpusPacketLossPercent(int percent)
{
    opusPacketLossPercent = percent;

    if (encoder)
        encoder->setPacketLossPercent(percent);
}

void AudioPipeline::onAudioCaptured(const QByteArray &pcmData)
{
    if (!encoder)
        return;

    QByteArray frame = pcmData;
    float voiceProb = -1.0f;
    if (noiseSuppressor && (noiseSuppressionEnabled || useRnnoiseVad)) {
        QByteArray denoised = noiseSuppressor->process(pcmData, voiceProb);
        if (noiseSuppressionEnabled)
            frame = denoised;
    }

    float rms = 0.0f;
    bool rmsVoice = detectVoiceActivity(frame, rms);

    bool voiceDetected;
    if (useRnnoiseVad && voiceProb >= 0.0f)
        voiceDetected = voiceProb > vadProbabilityThreshold;
    else
        voiceDetected = rmsVoice;

    if (rmsThrottleTimer.elapsed() >= RMS_EMIT_INTERVAL_MS) {
        emit audioLevelChanged(rms);
        rmsThrottleTimer.restart();
    }

    if (voiceDetected) {
        vadHoldoffCounter = vadHoldoffFrames;
        if (!isSpeaking) {
            isSpeaking = true;
            emit speakingChanged(true);
        }
    } else if (vadHoldoffCounter > 0) {
        vadHoldoffCounter--;
    } else if (isSpeaking) {
        sendTrailingSilence();
        isSpeaking = false;
        emit speakingChanged(false);
        return;
    }

    if (!isSpeaking)
        return;

    QByteArray encoded = encoder->encode(frame);
    if (!encoded.isEmpty())
        emit encodedAudioReady(encoded);
}

void AudioPipeline::onMixTick()
{
    expireRemoteSpeaking();
    if (deafened || !audioBackend)
        return;

    QVector<std::pair<QByteArray, float>> streams;

    for (auto it = speakers.begin(); it != speakers.end(); ++it) {
        const quint32 source = speakerSource(it->first);
        const quint32 ssrc = speakerSsrc(it->first);
        SpeakerState &state = it->second;

        QByteArray pcm;

        if (!state.pendingFrames.isEmpty()) {
            pcm = state.pendingFrames.takeFirst();
        } else {
            if (!state.jitterBuffer->isReady())
                continue;

            QByteArray opusData = state.jitterBuffer->pop();

            if (opusData.isEmpty()) {
                pcm = state.decoder->decodePlc();
            } else {
                QVector<QByteArray> frames = state.decoder->decode(opusData);
                if (frames.isEmpty())
                    continue;
                pcm = frames.first();
                for (int i = 1; i < frames.size(); i++)
                    state.pendingFrames.append(frames[i]);
            }
        }

        if (pcm.isEmpty())
            continue;

        // rms before gain
        auto userIt = source == VOICE_CONNECTION_SOURCE ? ssrcToUser.constFind(ssrc) : ssrcToUser.constEnd();
        Snowflake userId;
        if (userIt != ssrcToUser.constEnd())
            userId = userIt.value();

        if (userId.isValid()) {
            const auto *samples = reinterpret_cast<const int16_t *>(pcm.constData());
            int count = pcm.size() / static_cast<int>(sizeof(int16_t));
            if (count > 0) {
                float rms = rmsOf(samples, count);

                auto rmsIt = pendingUserRms.find(userId);
                if (rmsIt == pendingUserRms.end())
                    pendingUserRms.insert(userId, rms);
                else if (rms > rmsIt.value())
                    rmsIt.value() = rms;
            }
        }

        float gain = 1.0f;
        if (userId.isValid())
            gain = userVolumes.value(userId, 1.0f);
        else if (source != VOICE_CONNECTION_SOURCE)
            gain = sourceVolumes.value(source, 1.0f);

        streams.append({ pcm, gain });
    }

    // emit periodically
    if (userRmsThrottleTimer.elapsed() >= RMS_EMIT_INTERVAL_MS) {
        for (auto it = pendingUserRms.constBegin(); it != pendingUserRms.constEnd(); ++it)
            emit userAudioLevelChanged(it.key(), it.value());
        pendingUserRms.clear();
        userRmsThrottleTimer.restart();
    }

    if (streams.isEmpty())
        return;

    QByteArray mixed = AudioMixer::mix(streams);
    if (mixed.isEmpty())
        return;

    audioBackend->pushPlaybackFrame(reinterpret_cast<const int16_t *>(mixed.constData()));
}

bool AudioPipeline::detectVoiceActivity(const QByteArray &pcmFrame, float &outRms) const
{
    const auto *samples = reinterpret_cast<const int16_t *>(pcmFrame.constData());
    int count = pcmFrame.size() / static_cast<int>(sizeof(int16_t));
    if (count == 0) {
        outRms = 0.0f;
        return false;
    }

    outRms = rmsOf(samples, count);
    return outRms > vadThreshold;
}

void AudioPipeline::sendTrailingSilence()
{
    QByteArray silence(reinterpret_cast<const char *>(OPUS_SILENCE), sizeof(OPUS_SILENCE));
    for (int i = 0; i < TRAILING_SILENCE_FRAMES; i++)
        emit encodedAudioReady(silence);
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
