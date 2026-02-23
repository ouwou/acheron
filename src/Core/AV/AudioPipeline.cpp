#include "AudioPipeline.hpp"
#include "IAudioBackend.hpp"
#include "OpusEncoder.hpp"
#include "OpusDecoder.hpp"
#include "JitterBuffer.hpp"
#include "AudioMixer.hpp"

#include "Core/Logging.hpp"

#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Acheron {
namespace Core {
namespace AV {

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

    encoder = std::make_unique<OpusEncoder>();
    if (!encoder->init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS)) {
        qCCritical(LogVoice) << "Failed to initialize Opus encoder";
        encoder.reset();
    }

    rmsThrottleTimer.start();
    userRmsThrottleTimer.start();

    if (capturing)
        audioBackend->startCapture();

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

    encoder.reset();

    audioBackend = nullptr;

    isSpeaking = false;
    vadHoldoffCounter = 0;

    qCDebug(LogVoice) << "Audio pipeline stopped";
}

void AudioPipeline::startCapture()
{
    if (audioBackend)
        audioBackend->startCapture();
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

void AudioPipeline::onAudioReceived(quint32 ssrc, uint16_t sequence, uint32_t /*timestamp*/, const QByteArray &opusData)
{
    auto it = speakers.find(ssrc);
    if (it == speakers.end()) {
        SpeakerState state;
        state.decoder = std::make_unique<OpusDecoder>();
        if (!state.decoder->init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS)) {
            qCWarning(LogVoice) << "Failed to init decoder for SSRC" << ssrc;
            return;
        }
        state.jitterBuffer = std::make_unique<JitterBuffer>();

        auto [inserted, _] = speakers.emplace(ssrc, std::move(state));
        it = inserted;
    }

    it->second.jitterBuffer->push(sequence, opusData);
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
            speakers.erase(it.key());
            it = ssrcToUser.erase(it);
        } else {
            ++it;
        }
    }
    userVolumes.remove(userId);
    pendingUserRms.remove(userId);
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
    if (audioBackend)
        audioBackend->setInputDevice(deviceId);
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

void AudioPipeline::onAudioCaptured(const QByteArray &pcmData)
{
    if (!encoder)
        return;

    float rms = 0.0f;
    bool voiceDetected = detectVoiceActivity(pcmData, rms);

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

    QByteArray encoded = encoder->encode(pcmData);
    if (!encoded.isEmpty())
        emit encodedAudioReady(encoded);
}

void AudioPipeline::onMixTick()
{
    if (deafened || !audioBackend)
        return;

    QVector<std::pair<QByteArray, float>> streams;

    for (auto it = speakers.begin(); it != speakers.end(); ++it) {
        quint32 ssrc = it->first;
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
        auto userIt = ssrcToUser.constFind(ssrc);
        Snowflake userId;
        if (userIt != ssrcToUser.constEnd())
            userId = userIt.value();

        if (userId.isValid()) {
            const auto *samples = reinterpret_cast<const int16_t *>(pcm.constData());
            int count = pcm.size() / static_cast<int>(sizeof(int16_t));
            if (count > 0) {
                float rms = computeRms(samples, count);

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

    outRms = computeRms(samples, count);
    return outRms > vadThreshold;
}

float AudioPipeline::computeRms(const int16_t *samples, int count)
{
    double sum = 0;
    for (int i = 0; i < count; i++)
        sum += static_cast<double>(samples[i]) * samples[i];
    return static_cast<float>(std::sqrt(sum / count));
}

void AudioPipeline::sendTrailingSilence()
{
    QByteArray silence(reinterpret_cast<const char *>(OPUS_SILENCE), sizeof(OPUS_SILENCE));
    for (int i = 0; i < TRAILING_SILENCE_FRAMES; i++)
        emit encodedAudioReady(silence);
}

} // namespace AV
} // namespace Core
} // namespace Acheron
