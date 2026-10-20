#include "Core/Audio/LoopbackCapture.hpp"

#include <optional>

namespace Acheron {
namespace Core {
namespace Audio {

// Where nothing can be captured. The capture is in LoopbackCaptureWindows.cpp and
// LoopbackCapturePipeWire.cpp.
#if !defined(Q_OS_WIN) && !defined(ACHERON_HAVE_PIPEWIRE)

class LoopbackCapture::Worker
{
public:
    explicit Worker(LoopbackCapture *owner) : owner(owner) {}

    void request(const std::optional<Target> &target)
    {
        if (target)
            QMetaObject::invokeMethod(owner, [owner = owner] { emit owner->failed(LoopbackCapture::tr("This build cannot capture what a program is playing.")); }, Qt::QueuedConnection);
    }

private:
    LoopbackCapture *owner;
};

bool processLoopbackAvailable()
{
    return false;
}

QList<PlayingProgram> playingPrograms()
{
    return {};
}

LoopbackCapture::LoopbackCapture(QObject *parent) : QObject(parent), worker(std::make_unique<Worker>(this)) {}

LoopbackCapture::~LoopbackCapture() = default;

void LoopbackCapture::start(quint32 processId, Scope scope)
{
    worker->request(Target{ processId, scope });
}

void LoopbackCapture::stop()
{
    worker->request(std::nullopt);
}

#endif

} // namespace Audio
} // namespace Core
} // namespace Acheron
