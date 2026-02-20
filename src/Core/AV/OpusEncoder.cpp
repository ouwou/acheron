#include "OpusEncoder.hpp"
#include "IAudioBackend.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Core {
namespace AV {

static constexpr int OPUS_MAX_PACKET_SIZE = 4000;

OpusEncoder::OpusEncoder() = default;

OpusEncoder::~OpusEncoder()
{
    if (encoder)
        opus_encoder_destroy(encoder);
}

bool OpusEncoder::init(int sampleRate, int channels)
{
    frameSamples = sampleRate * AUDIO_FRAME_DURATION_MS / 1000;
    frameChannels = channels;

    int error;
    encoder = opus_encoder_create(sampleRate, channels, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !encoder) {
        qCCritical(LogVoice) << "Failed to create Opus encoder:" << opus_strerror(error);
        return false;
    }

    setBitrate(64000);
    setComplexity(5);
    setSignalType(OPUS_SIGNAL_VOICE);
    setFec(true);
    setDtx(false);
    setPacketLossPercent(0);
    return true;
}

QByteArray OpusEncoder::encode(const QByteArray &pcmFrame)
{
    if (!encoder)
        return {};

    int expectedSize = frameSamples * frameChannels * static_cast<int>(sizeof(opus_int16));
    if (pcmFrame.size() != expectedSize) {
        qCWarning(LogVoice) << "OpusEncoder: unexpected PCM size" << pcmFrame.size()
                            << "expected" << expectedSize;
        return {};
    }

    QByteArray encoded(OPUS_MAX_PACKET_SIZE, '\0');

    int bytes = opus_encode(encoder,
                            reinterpret_cast<const opus_int16 *>(pcmFrame.constData()),
                            frameSamples,
                            reinterpret_cast<unsigned char *>(encoded.data()),
                            OPUS_MAX_PACKET_SIZE);

    if (bytes < 0) {
        qCWarning(LogVoice) << "Opus encode failed:" << opus_strerror(bytes);
        return {};
    }

    encoded.resize(bytes);
    return encoded;
}

void OpusEncoder::setBitrate(int bitrate)
{
    if (encoder)
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
}

void OpusEncoder::setComplexity(int complexity)
{
    if (encoder)
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(complexity));
}

void OpusEncoder::setSignalType(int signalType)
{
    if (encoder)
        opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(signalType));
}

void OpusEncoder::setFec(bool enabled)
{
    if (encoder)
        opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(enabled ? 1 : 0));
}

void OpusEncoder::setDtx(bool enabled)
{
    if (encoder)
        opus_encoder_ctl(encoder, OPUS_SET_DTX(enabled ? 1 : 0));
}

void OpusEncoder::setPacketLossPercent(int percent)
{
    if (encoder)
        opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(percent));
}

} // namespace AV
} // namespace Core
} // namespace Acheron
