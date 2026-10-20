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

// Which video codecs are offered to the voice server, for what this client receives and for what
// it sends. Lists of codecs are in the order the server prefers them: AV1, H265, H264, VP8.

// The codecs video can be sent with: those this machine has an encoder for that works, less those
// the settings window turned off. Finding the encoders out opens each of them once, which is too
// slow for the UI thread, so the answer comes later (at once when it is known already), on the
// main thread, unless `context` has gone by then. Call it from the main thread.
void whenSendCodecsKnown(QObject *context, std::function<void(const QList<Discord::Voice::VideoCodec> &)> then);

// Whether the user lets video be sent with a codec: every one, unless the settings window turned
// it off (`video/send_codecs_off`, by name). The server chooses among those that are offered, so
// what is turned off here is never chosen.
[[nodiscard]] bool sendCodecAllowed(Discord::Voice::VideoCodec codec);
void setSendCodecAllowed(Discord::Voice::VideoCodec codec, bool allowed);
[[nodiscard]] QList<Discord::Voice::VideoCodec> allowedSendCodecs(const QList<Discord::Voice::VideoCodec> &encodable);

// What a screen or window share whose pictures are `size` offers to send with, of `sendable`:
// without AV1 when `av1Encoder` adds more to that size than is acceptable.
[[nodiscard]] QList<Discord::Voice::VideoCodec> shareCodecs(const QList<Discord::Voice::VideoCodec> &sendable, const QSize &size, const QString &av1Encoder = videoEncoderFor(Discord::Voice::VideoCodec::AV1));
// The same for the camera, whose size follows what its viewers want and cannot be told
// beforehand: AV1 only from an encoder that codes every size as it is.
[[nodiscard]] QList<Discord::Voice::VideoCodec> cameraCodecs(const QList<Discord::Voice::VideoCodec> &sendable, const QString &av1Encoder = videoEncoderFor(Discord::Voice::VideoCodec::AV1));

// A SELECT_PROTOCOL codec list, whose payload types are positional: every codec this machine
// decodes is offered for decoding, and those in `encodable` for sending as well.
[[nodiscard]] QList<Discord::Voice::Codec> offeredVideoCodecs(const QList<Discord::Voice::VideoCodec> &encodable = {});

} // namespace Media
} // namespace Core
} // namespace Acheron
