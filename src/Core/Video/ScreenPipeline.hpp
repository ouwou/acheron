#pragma once

#include <QString>

#include <memory>
#include <optional>

#include "Core/Video/CaptureSources.hpp"
#include "Core/Video/EncodePipeline.hpp"

namespace Acheron {
namespace Core {
namespace Video {

struct ScreenSettings
{
    QString sourceId;
    std::optional<int> maxHeight = 720;
    int framerate = 30;
    Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown;
    std::optional<int> bitrateBps;
    std::optional<int> maxBitrateBps;
};

class ScreenPipeline : public EncodePipeline
{
    Q_OBJECT
public:
    explicit ScreenPipeline(QObject *parent = nullptr);
    ~ScreenPipeline() override;

    // all of these may be called from any thread and return at once
    void start(const ScreenSettings &settings);
    void stop();
    void requestKeyframe();
    void setBitrate(int bitrateBps);
    void setEncodingSuspended(bool suspended);

signals:
    void captureStalled(bool stalled);
    void sourceClosed();

private:
    class Worker;
    std::unique_ptr<Worker> worker;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
