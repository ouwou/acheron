#pragma once

#include <QString>

#include <memory>

#include "Core/Video/CaptureSources.hpp"
#include "Core/Video/EncodePipeline.hpp"

namespace Acheron {
namespace Core {
namespace Video {

struct ScreenSettings
{
    QString sourceId;
    // The tallest picture to send; a taller source is scaled down keeping its shape. Zero
    // sends the source at its own size.
    int maxHeight = 720;
    int framerate = 30;
    Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown;
    int bitrateBps = 0;
    int maxBitrateBps = 0;
    // From the server; zero means key frames are made only when asked for.
    int keyframeIntervalMs = 0;
};

// Captures a screen or a window, scales it and encodes it, in one layer.
//
// A source whose size the codec's encoder cannot code acceptably (Media::paddingIsAcceptable)
// is captured and reported and not encoded: started() says the size, and nothing follows
// until the pipeline is started again with another codec or the source changes its size.
class ScreenPipeline : public EncodePipeline
{
    Q_OBJECT
public:
    explicit ScreenPipeline(QObject *parent = nullptr);
    ~ScreenPipeline() override;

    // All of these may be called from any thread and return at once. The capture method is
    // the one the settings name as the pipeline starts.
    void start(const ScreenSettings &settings);
    void stop();
    // The first frame encoded after this is a key frame.
    void requestKeyframe();
    // Encoders that cannot follow a new rate while running are reopened, which costs a key
    // frame, so callers should move this in steps and not continuously.
    void setBitrate(int bitrateBps);
    // While nobody is watching: nothing is encoded, and the source is read only as often as
    // the previews need it. Sending again starts with a key frame.
    void setEncodingSuspended(bool suspended);

signals:
    // The source stopped delivering pictures (true) or resumed (false).
    void captureStalled(bool stalled);
    // The window being shared was closed. Nothing more will come, and nothing went wrong.
    void sourceClosed();

private:
    class Worker;
    std::unique_ptr<Worker> worker;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
