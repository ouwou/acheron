#include "Core/Media/CodecSupport.hpp"

#include "Core/Media/VideoDecoder.hpp"
#include "Core/Media/VideoEncoder.hpp"

#include <QCoreApplication>
#include <QPointer>
#include <QSettings>
#include <QThread>

#include <algorithm>
#include <array>
#include <thread>
#include <utility>

namespace Acheron {
namespace Core {
namespace Media {

using Discord::Voice::VideoCodec;

namespace {

const char *const SEND_CODECS_OFF_KEY = "video/send_codecs_off";
const char *const RECEIVE_CODECS_OFF_KEY = "video/receive_codecs_off";
constexpr int FIRST_PAYLOAD_TYPE = 101;

constexpr std::array SIZES_NOT_OF_WHOLE_BLOCKS = { QSize(640, 360), QSize(1000, 700) };

using SendCodecsKnown = std::function<void(const QList<VideoCodec> &)>;

struct EncoderProbe
{
    bool known = false;
    bool running = false;
    QList<VideoCodec> encodable;
    QList<std::pair<QPointer<QObject>, SendCodecsKnown>> waiting;
};

EncoderProbe &encoderProbe()
{
    Q_ASSERT(QThread::currentThread() == qApp->thread());
    static EncoderProbe probe;
    return probe;
}

} // namespace

void startSendCodecProbe()
{
    whenSendCodecsKnown(nullptr, [](const QList<VideoCodec> &) {});
}

void whenSendCodecsKnown(QObject *context, SendCodecsKnown then)
{
    EncoderProbe &probe = encoderProbe();
    if (probe.known) {
        then(allowedSendCodecs(probe.encodable));
        return;
    }

    probe.waiting.append({ QPointer<QObject>(context), std::move(then) });
    if (probe.running)
        return;
    probe.running = true;

    // not the thread pool, which the application waits for as it quits: a trial can take seconds
    std::thread([] {
        const QList<VideoCodec> encodable = encodableVideoCodecs();
        QMetaObject::invokeMethod(
                qApp,
                [encodable] {
                    EncoderProbe &probe = encoderProbe();
                    probe.known = true;
                    probe.encodable = encodable;
                    const auto waiting = std::exchange(probe.waiting, {});
                    for (const auto &[context, then] : waiting) {
                        if (context)
                            then(allowedSendCodecs(encodable));
                    }
                },
                Qt::QueuedConnection);
    }).detach();
}

namespace {

QStringList codecsTurnedOff(const char *key)
{
    return QSettings().value(key).toStringList();
}

void setCodecTurnedOff(const char *key, VideoCodec codec, bool turnedOff)
{
    const QString name = Discord::Voice::videoCodecName(codec);
    QStringList off = codecsTurnedOff(key);
    off.removeAll(name);
    if (turnedOff)
        off.append(name);
    QSettings().setValue(key, off);
}

} // namespace

bool sendCodecAllowed(VideoCodec codec)
{
    return !codecsTurnedOff(SEND_CODECS_OFF_KEY).contains(Discord::Voice::videoCodecName(codec));
}

void setSendCodecAllowed(VideoCodec codec, bool allowed)
{
    setCodecTurnedOff(SEND_CODECS_OFF_KEY, codec, !allowed);
}

bool receiveCodecAllowed(VideoCodec codec)
{
    return !codecsTurnedOff(RECEIVE_CODECS_OFF_KEY).contains(Discord::Voice::videoCodecName(codec));
}

void setReceiveCodecAllowed(VideoCodec codec, bool allowed)
{
    setCodecTurnedOff(RECEIVE_CODECS_OFF_KEY, codec, !allowed);
}

QList<VideoCodec> allowedSendCodecs(const QList<VideoCodec> &encodable)
{
    const QStringList off = codecsTurnedOff(SEND_CODECS_OFF_KEY);
    QList<VideoCodec> allowed;
    for (VideoCodec codec : encodable) {
        if (!off.contains(Discord::Voice::videoCodecName(codec)))
            allowed.append(codec);
    }
    return allowed;
}

QList<VideoCodec> shareCodecs(const QList<VideoCodec> &sendable, const QSize &size, const QString &av1Encoder)
{
    QList<VideoCodec> codecs = sendable;
    if (!paddingIsAcceptable(size, codedSize(av1Encoder, size)))
        codecs.removeAll(VideoCodec::AV1);
    return codecs;
}

QList<VideoCodec> cameraCodecs(const QList<VideoCodec> &sendable, const QString &av1Encoder)
{
    QList<VideoCodec> codecs = sendable;
    if (std::any_of(SIZES_NOT_OF_WHOLE_BLOCKS.begin(), SIZES_NOT_OF_WHOLE_BLOCKS.end(), [&av1Encoder](const QSize &size) { return codedSize(av1Encoder, size) != size; }))
        codecs.removeAll(VideoCodec::AV1);
    return codecs;
}

QList<Discord::Voice::Codec> offeredVideoCodecs(const QList<VideoCodec> &encodable)
{
    QList<Discord::Voice::Codec> offered;
    const QList<VideoCodec> codecs = decodableVideoCodecs();
    const QStringList notReceived = codecsTurnedOff(RECEIVE_CODECS_OFF_KEY);
    for (int i = 0; i < codecs.size(); i++) {
        Discord::Voice::Codec codec;
        codec.name = Discord::Voice::videoCodecName(codecs[i]);
        codec.type = QStringLiteral("video");
        codec.priority = (i + 1) * 1000;
        codec.payloadType = FIRST_PAYLOAD_TYPE + 2 * i;
        codec.rtxPayloadType = FIRST_PAYLOAD_TYPE + 2 * i + 1;
        codec.encode = encodable.contains(codecs[i]);
        codec.decode = !notReceived.contains(Discord::Voice::videoCodecName(codecs[i])) && !decodingGivenUpOn(codecs[i]);
        offered.append(codec);
    }
    return offered;
}

} // namespace Media
} // namespace Core
} // namespace Acheron
