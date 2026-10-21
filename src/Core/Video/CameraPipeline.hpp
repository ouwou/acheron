#pragma once

#include <QList>
#include <QSize>
#include <QString>

#include <memory>
#include <optional>

#include "Core/Video/EncodePipeline.hpp"
#include "Core/Video/SendQuality.hpp"

namespace Acheron {
namespace Core {
namespace Video {

struct CameraDevice
{
    QString id;
    QString name;
};

[[nodiscard]] bool cameraCaptureAvailable();
[[nodiscard]] QList<CameraDevice> cameraDevices();
[[nodiscard]] QString preferredCamera(const QList<CameraDevice> &attached);
void setPreferredCamera(const QString &deviceId);

struct CameraSettings
{
    QString deviceId;
    Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown;
    QSize encodeSize = QSize(1280, 720);
    int framerate = 30;
    std::optional<int> bitrateBps;
    std::optional<int> maxBitrateBps;
};

class CameraPipeline : public EncodePipeline
{
    Q_OBJECT
public:
    explicit CameraPipeline(QObject *parent = nullptr);
    ~CameraPipeline() override;

    // all of these may be called from any thread and return at once
    void start(const CameraSettings &settings);
    void stop();
    void requestKeyframe(int layer = CAMERA_FULL_LAYER);
    // an encoder that cant follow a rate live is reopened, costing a key frame, so move this in steps
    void setBitrate(int bitrateBps, int layer = CAMERA_FULL_LAYER);
    void setEncoding(const QSize &encodeSize, int framerate, int layer = CAMERA_FULL_LAYER);
    void setEncodingSuspended(bool suspended, int layer = CAMERA_FULL_LAYER);

private:
    class Worker;
    std::unique_ptr<Worker> worker;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
