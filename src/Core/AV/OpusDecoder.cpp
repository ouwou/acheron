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
    frameSamples = sampleRate * AUDIO_FRAME_DURATION_MS / 1000;
    frameChannels = channels;

    int error;
    decoder = opus_decoder_create(sampleRate, channels, &error);
    if (error != OPUS_OK || !decoder) {
        qCCritical(LogVoice) << "Failed to create Opus decoder:" << opus_strerror(error);
        return false;
    }
    return true;
}

QByteArray OpusDecoder::decode(const QByteArray &opusData)
{
    if (!decoder)
        return {};

    int pcmSize = frameSamples * frameChannels * static_cast<int>(sizeof(opus_int16));
    QByteArray pcm(pcmSize, '\0');

    int samples = opus_decode(decoder,
                              reinterpret_cast<const unsigned char *>(opusData.constData()),
                              opusData.size(),
                              reinterpret_cast<opus_int16 *>(pcm.data()),
                              frameSamples,
                              0);

    if (samples < 0) {
        qCDebug(LogVoice) << "Opus decode failed:" << opus_strerror(samples);
        return {};
    }

    pcm.resize(samples * frameChannels * static_cast<int>(sizeof(opus_int16)));
    return pcm;
}

QByteArray OpusDecoder::decodePlc()
{
    if (!decoder)
        return {};

    int pcmSize = frameSamples * frameChannels * static_cast<int>(sizeof(opus_int16));
    QByteArray pcm(pcmSize, '\0');

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

} // namespace AV
} // namespace Core
} // namespace Acheron
