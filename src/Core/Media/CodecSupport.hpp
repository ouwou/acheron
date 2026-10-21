#pragma once

#include <QList>
#include <QSize>

#include <functional>

#include "Core/Media/VideoEncoder.hpp"
#include "Discord/Voice/VideoFrame.hpp"
#include "Discord/Voice/VoiceEntities.hpp"

class QObject;

namespace Acheron {
namespace Core {
namespace Media {

// too slow for the UI thread: `then` runs later on the main thread, unless `context` is gone
void whenSendCodecsKnown(QObject *context, std::function<void(const QList<Discord::Voice::VideoCodec> &)> then);
void startSendCodecProbe();

// answers only the latest ask
class SendCodecRequest
{
public:
    void ask(QObject *context, std::function<void(const QList<Discord::Voice::VideoCodec> &)> then)
    {
        whenSendCodecsKnown(context, [this, asked = ++attempt, then = std::move(then)](const QList<Discord::Voice::VideoCodec> &codecs) {
            if (asked == attempt)
                then(codecs);
        });
    }
    void cancel() { attempt++; }

private:
    quint64 attempt = 0;
};

[[nodiscard]] bool sendCodecAllowed(Discord::Voice::VideoCodec codec);
void setSendCodecAllowed(Discord::Voice::VideoCodec codec, bool allowed);
[[nodiscard]] QList<Discord::Voice::VideoCodec> allowedSendCodecs(const QList<Discord::Voice::VideoCodec> &encodable);

[[nodiscard]] QList<Discord::Voice::VideoCodec> shareCodecs(const QList<Discord::Voice::VideoCodec> &sendable, const QSize &size, const QString &av1Encoder = videoEncoderFor(Discord::Voice::VideoCodec::AV1));
[[nodiscard]] QList<Discord::Voice::VideoCodec> cameraCodecs(const QList<Discord::Voice::VideoCodec> &sendable, const QString &av1Encoder = videoEncoderFor(Discord::Voice::VideoCodec::AV1));

// SELECT_PROTOCOL's payload types are positional
[[nodiscard]] QList<Discord::Voice::Codec> offeredVideoCodecs(const QList<Discord::Voice::VideoCodec> &encodable = {});

} // namespace Media
} // namespace Core
} // namespace Acheron
