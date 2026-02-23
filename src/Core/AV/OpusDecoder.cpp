#include "OpusDecoder.hpp"
#include "IAudioBackend.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Core {
namespace AV {

OpusDecoder::OpusDecoder() = default;

OpusDecoder::~OpusDecoder()
{
    if (decoder)
        opus_decoder_destroy(decoder);
}

bool OpusDecoder::init(int sampleRate, int channels)
{
    if (decoder) {
        opus_decoder_destroy(decoder);
        decoder = nullptr;
    }

    frameSamples = sampleRate * AUDIO_FRAME_DURATION_MS / 1000;
    frameChannels = channels;
    frameBytes = frameSamples * frameChannels * static_cast<int>(sizeof(opus_int16));

    int error;
    decoder = opus_decoder_create(sampleRate, channels, &error);
    if (error != OPUS_OK || !decoder) {
        qCCritical(LogVoice) << "Failed to create Opus decoder:" << opus_strerror(error);
        return false;
    }
    return true;
}

QVector<QByteArray> OpusDecoder::decode(const QByteArray &opusData)
{
    if (!decoder)
        return {};

    // 120ms * 48khz
    static constexpr int MAX_OPUS_SAMPLES = 5760;
    int pcmSize = MAX_OPUS_SAMPLES * frameChannels * static_cast<int>(sizeof(opus_int16));
    QByteArray pcm(pcmSize, '\0');

    int samples = opus_decode(decoder,
                              reinterpret_cast<const unsigned char *>(opusData.constData()),
                              opusData.size(),
                              reinterpret_cast<opus_int16 *>(pcm.data()),
                              MAX_OPUS_SAMPLES,
                              0);

    if (samples < 0) {
        qCDebug(LogVoice) << "Opus decode failed:" << opus_strerror(samples);
        return {};
    }

    return splitFrames(pcm, samples);
}

QByteArray OpusDecoder::decodePlc()
{
    if (!decoder)
        return {};

    QByteArray pcm(frameBytes, '\0');

    int samples = opus_decode(decoder,
                              nullptr, 0,
                              reinterpret_cast<opus_int16 *>(pcm.data()),
                              frameSamples,
                              0);

    if (samples < 0) {
        qCDebug(LogVoice) << "Opus PLC failed:" << opus_strerror(samples);
        return {};
    }

    pcm.resize(samples * frameChannels * static_cast<int>(sizeof(opus_int16)));
    return pcm;
}

QVector<QByteArray> OpusDecoder::splitFrames(const QByteArray &pcm, int totalSamples)
{
    QVector<QByteArray> frames;

    int offset = 0;
    int remaining = totalSamples;

    while (remaining >= frameSamples) {
        frames.append(pcm.mid(offset, frameBytes));
        offset += frameBytes;
        remaining -= frameSamples;
    }

    if (remaining > 0) {
        qCDebug(LogVoice) << "Opus decoded partial tail:" << remaining << "samples";
    }

    return frames;
}

} // namespace AV
} // namespace Core
} // namespace Acheron
