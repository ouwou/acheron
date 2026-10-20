#include "UI/Call/CallStats.hpp"

#include "UI/Video/VideoSurface.hpp"

namespace Acheron {
namespace UI {

namespace {

QStringList sendLines(const Core::Video::PipelineStats &encoder, const Discord::Voice::VideoSendStats &network, int targetBitrate, int receivers)
{
    QStringList lines;
    const QString encoderName = encoder.encoderName.isEmpty() ? CallStats::tr("starting") : encoder.encoderName;
    lines << (network.codec.isEmpty() ? CallStats::tr("Encoder: %1").arg(encoderName) : CallStats::tr("Encoder: %1 by %2").arg(network.codec, encoderName));
    lines << CallStats::tr("Resolution: %1x%2").arg(encoder.encodedSize.width()).arg(encoder.encodedSize.height());
    lines << CallStats::tr("Frame rate: %1 fps encoded, %2 captured").arg(encoder.encodedFps, 0, 'f', 1).arg(encoder.capturedFps, 0, 'f', 1);
    lines << CallStats::tr("Bitrate: %1 kbps sent, %2 kbps asked of the encoder").arg(network.bitrateBps / 1000).arg(targetBitrate / 1000);
    lines << (receivers < 0 ? CallStats::tr("Viewers: not reported yet") : CallStats::tr("Viewers: %1").arg(receivers));
    lines << (network.lossFraction < 0 ? CallStats::tr("Loss: not reported yet") : CallStats::tr("Loss: %1%").arg(network.lossFraction * 100.0, 0, 'f', 1));
    if (network.receiverEstimateBps > 0)
        lines << CallStats::tr("Receiver's estimate: %1 kbps").arg(network.receiverEstimateBps / 1000);
    if (network.delayEstimateBps > 0)
        lines << CallStats::tr("Estimate from delays: %1 kbps, of %2 kbps acknowledged").arg(network.delayEstimateBps / 1000).arg(network.acknowledgedBps / 1000);
    return lines;
}

QString drawingOf(const VideoSurface *surface)
{
    if (!surface->drawsWithGpu())
        return CallStats::tr("software");
    return surface->drawsFromCard() ? CallStats::tr("graphics card, no copy") : CallStats::tr("graphics card");
}

} // namespace

QStringList CallStats::ofOwnShare(const Core::StreamBroadcaster::Stats &stats)
{
    QStringList lines = sendLines(stats.encoder, stats.network, stats.targetBitrate, stats.receivers);
    if (stats.audio == Core::StreamBroadcaster::AudioState::Capturing)
        lines << tr("Sound: shared");
    else if (stats.audio == Core::StreamBroadcaster::AudioState::Failed)
        lines << tr("Sound: could not be captured, sharing without it");
    if (stats.encodingSuspended)
        lines << tr("Encoding suspended: nobody is watching");
    if (stats.captureStalled)
        lines << tr("Capture stalled");
    return lines;
}

QStringList CallStats::ofOwnCamera(const Core::CameraSender::Stats &stats)
{
    QStringList lines = sendLines(stats.encoder, stats.network, stats.targetBitrate, stats.receivers);
    lines << tr("Wanted by viewers: %1%").arg(stats.servedWant);
    if (stats.mutedFramerate)
        lines << tr("Muted frame rate");
    if (stats.encodingSuspended)
        lines << tr("Encoding suspended: nobody is watching");
    if (stats.hasSecondLayer && stats.secondEncodingSuspended)
        lines << tr("Second layer: not encoded, nobody is watching it");
    else if (stats.hasSecondLayer)
        lines << tr("Second layer: %1x%2, %3 kbps asked of the encoder, wanted %4%, viewers %5").arg(stats.secondEncoder.encodedSize.width()).arg(stats.secondEncoder.encodedSize.height()).arg(stats.secondTargetBitrate / 1000).arg(stats.secondServedWant).arg(stats.secondReceivers < 0 ? tr("not reported yet") : QString::number(stats.secondReceivers));
    return lines;
}

QStringList CallStats::ofStream(const Core::StreamSession::Stats &stats, const VideoSurface *surface)
{
    QStringList lines;
    lines << tr("Codec: %1 (%2)").arg(stats.network.codec, stats.decoder.decoderName);
    lines << tr("Resolution: %1x%2").arg(stats.decoder.width).arg(stats.decoder.height);
    lines << tr("Frame rate: %1 fps").arg(stats.framesPerSecond, 0, 'f', 1);
    lines << tr("Bitrate: %1 kbps").arg(stats.bitrateKbps, 0, 'f', 0);
    lines << tr("Decode time: %1 ms").arg(stats.decoder.averageDecodeMs, 0, 'f', 2);
    if (stats.pixelShareAskedFor < 1)
        lines << tr("Decoder behind: asking for %1% of the picture").arg(qRound(stats.pixelShareAskedFor * 100));
    lines << tr("Loss: %1%").arg(stats.network.lossFraction * 100.0, 0, 'f', 1);
    if (stats.network.serverEstimateBps != 0)
        lines << tr("Server estimate: %1 kbps, %2% loss").arg(stats.network.serverEstimateBps / 1000).arg(stats.network.serverLossFraction * 100.0, 0, 'f', 1);
    lines << tr("NACK: %1, PLI: %2").arg(stats.network.nacked).arg(stats.network.keyframeRequests);
    lines << tr("Frames: %1 decoded, %2 dropped, %3 errors").arg(stats.decoder.decoded).arg(stats.decoder.dropped).arg(stats.decoder.errors);
    if (stats.network.decryptFailures != 0)
        lines << tr("Decrypt failures: %1").arg(stats.network.decryptFailures);
    lines << tr("Drawing: %1").arg(drawingOf(surface));
    return lines;
}

QStringList CallStats::ofCamera(const Core::Media::VideoDecoder::Stats &stats, const VideoSurface *surface)
{
    if (stats.decoderName.isEmpty())
        return {};
    return {
        tr("Decoder: %1").arg(stats.decoderName),
        tr("Resolution: %1x%2").arg(stats.width).arg(stats.height),
        tr("Decode time: %1 ms").arg(stats.averageDecodeMs, 0, 'f', 2),
        tr("Frames: %1 decoded, %2 dropped, %3 errors").arg(stats.decoded).arg(stats.dropped).arg(stats.errors),
        tr("Drawing: %1").arg(drawingOf(surface)),
    };
}

} // namespace UI
} // namespace Acheron
