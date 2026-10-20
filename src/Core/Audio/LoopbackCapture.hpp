#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>

namespace Acheron {
namespace Core {
namespace Audio {

// Whether this machine can capture what one program is playing, or everything but one program.
// Nothing offers to share sound where it cannot.
[[nodiscard]] bool processLoopbackAvailable();

struct PlayingProgram
{
    quint32 processId = 0;
    QString name;
};

// The programs that have a sound output open right now, this one left out. Empty where the
// platform has no way to tell.
[[nodiscard]] QList<PlayingProgram> playingPrograms();

// Captures what is being played, as 20 ms frames of 48 kHz stereo 16-bit PCM, on a thread of
// its own. Its signals are emitted from that thread.
class LoopbackCapture : public QObject
{
    Q_OBJECT
public:
    enum class Scope {
        // What this process and the ones it started are playing.
        ProcessTree,
        // What everything else is playing.
        AllButProcessTree,
    };
    struct Target
    {
        quint32 processId = 0;
        Scope scope = Scope::ProcessTree;
    };

    explicit LoopbackCapture(QObject *parent = nullptr);
    ~LoopbackCapture() override;

    // Both may be called from any thread and return at once. Starting again replaces what was
    // being captured, and a frame that was on its way may still arrive after stop().
    void start(quint32 processId, Scope scope);
    void stop();

signals:
    // Frames come for as long as anything at all is being played on the machine, and hold
    // silence (a little noise, not zeros) while none of it is in scope. A process that does
    // not exist is captured like one that plays nothing.
    void frameCaptured(const QByteArray &pcm);
    // The capture could not be made or has broken off. Nothing more comes of this start.
    void failed(const QString &reason);

private:
    class Worker;
    std::unique_ptr<Worker> worker;
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
