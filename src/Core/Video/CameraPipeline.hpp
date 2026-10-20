#pragma once

#include <QList>
#include <QSize>
#include <QString>

#include <memory>

#include "Core/Video/EncodePipeline.hpp"

namespace Acheron {
namespace Core {
namespace Video {

struct CameraDevice
{
    // Stays the same for a camera from one run to the next, and is what settings remember.
    QString id;
    QString name;
};

// False where this build cannot open cameras at all, in which case nothing offers one.
[[nodiscard]] bool cameraCaptureAvailable();
// The cameras attached now. Asking is cheap and opens none of them.
[[nodiscard]] QList<CameraDevice> cameraDevices();
// The camera the user chose last (`video/camera_device`), when it is among `attached`. Empty
// otherwise, which takes the first there is.
[[nodiscard]] QString preferredCamera(const QList<CameraDevice> &attached);
void setPreferredCamera(const QString &deviceId);

struct CameraSettings
{
    // Empty takes the first camera there is.
    QString deviceId;
    Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown;
    // What is encoded. The camera itself is always asked for 1280x720 at 30, or the nearest it
    // has, and its picture is scaled and its frames thinned to this.
    QSize encodeSize = QSize(1280, 720);
    int framerate = 30;
    int bitrateBps = 0;
    int maxBitrateBps = 0;
    // From the server; zero means key frames are made only when asked for.
    int keyframeIntervalMs = 0;
};

// Opens a camera and encodes its picture. Its previews are the camera's own picture for its
// owner to look at: no larger than 640x360, about fifteen times a second, not mirrored.
class CameraPipeline : public EncodePipeline
{
    Q_OBJECT
public:
    explicit CameraPipeline(QObject *parent = nullptr);
    ~CameraPipeline() override;

    // The camera's pictures can be encoded more than once, each layer at a size, frame rate
    // and bitrate of its own with an encoder of its own. The layers beside the first encode
    // nothing until they are taken out of suspension, and start as the first does unless they
    // are told otherwise first.
    static constexpr int LAYERS = 2;

    // All of these may be called from any thread and return at once.
    void start(const CameraSettings &settings);
    void stop();
    // The first frame the layer encodes after this is a key frame.
    void requestKeyframe(int layer = 0);
    // Encoders that cannot follow a new rate while running are reopened, which costs a key
    // frame, so callers should move this in steps and not continuously.
    void setBitrate(int bitrateBps, int layer = 0);
    // A new size or frame rate to encode at, the camera staying open. Starts with a key frame.
    void setEncoding(const QSize &encodeSize, int framerate, int layer = 0);
    // While nobody is watching: the camera stays open, so the preview stays live, and the
    // layer encodes nothing. Sending again starts with a key frame.
    void setEncodingSuspended(bool suspended, int layer = 0);

private:
    class Worker;
    std::unique_ptr<Worker> worker;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
