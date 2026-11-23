#include "StreamAudio.hpp"

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Audio/OpusEncoder.hpp"
#include "Discord/Voice/VoiceClient.hpp"

namespace Acheron {
namespace Core {

using Discord::Voice::VoiceClient;

StreamAudio::StreamAudio(VoiceClient *rtc, int bitrate)
    : rtc(rtc), encoder(std::make_unique<Audio::OpusEncoder>()), soundGone(this)
{
    if (encoder->init(Audio::AUDIO_SAMPLE_RATE, Audio::AUDIO_CHANNELS, OPUS_APPLICATION_AUDIO)) {
        encoder->setBitrate(bitrate);
        encoder->setSignalType(OPUS_SIGNAL_MUSIC);
        // Opus FEC belongs to its speech coder, which doesnt encode this
        encoder->setFec(false);
    } else {
        encoder.reset();
    }

    soundGone.setSingleShot(true);
    soundGone.setInterval(SOUND_GONE_MS);
    connect(&soundGone, &QTimer::timeout, this, &StreamAudio::onSoundGone);
}

StreamAudio::~StreamAudio() = default;

void StreamAudio::onFrameCaptured(const QByteArray &pcm)
{
    if (!encoder)
        return;

    if (rtc->state() != VoiceClient::State::Connected) {
        runs.stop();
        soundGone.stop();
        return;
    }

    switch (runs.offer(isSilence(pcm))) {
    case SoundRuns::Step::Skip:
        return;
    case SoundRuns::Step::End:
        soundGone.stop();
        sendTrailingSilence();
        return;
    case SoundRuns::Step::Begin:
        rtc->beginSoundshare();
        break;
    case SoundRuns::Step::Continue:
        break;
    }

    soundGone.start();
    const QByteArray opus = encoder->encode(pcm);
    if (!opus.isEmpty())
        rtc->sendAudio(opus);
}

void StreamAudio::onSoundGone()
{
    if (runs.stop())
        sendTrailingSilence();
}

void StreamAudio::sendTrailingSilence()
{
    const QByteArray silence(reinterpret_cast<const char *>(Audio::OPUS_SILENCE), sizeof(Audio::OPUS_SILENCE));
    for (int i = 0; i < TRAILING_SILENCE_FRAMES; i++)
        rtc->sendAudio(silence);
}

} // namespace Core
} // namespace Acheron
