#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>

namespace Acheron {
namespace Core {
namespace Audio {

[[nodiscard]] bool processLoopbackAvailable();

struct PlayingProgram
{
    quint32 processId = 0;
    QString name;
};

[[nodiscard]] QList<PlayingProgram> playingPrograms();

// captured on and emitted from a thread of its own
class LoopbackCapture : public QObject
{
    Q_OBJECT
public:
    enum class Scope {
        ProcessTree,
        AllButProcessTree,
    };
    struct Target
    {
        quint32 processId = 0;
        Scope scope = Scope::ProcessTree;
    };

    explicit LoopbackCapture(QObject *parent = nullptr);
    ~LoopbackCapture() override;

    // any thread. a frame already on its way may still arrive after stop()
    void start(quint32 processId, Scope scope);
    void stop();

signals:
    // frames come while anything at all plays on the machine, holding silence (noise, not zeros) while
    // nothing in scope does
    void frameCaptured(const QByteArray &pcm);
    void failed(const QString &reason);

private:
    class Worker;
    std::unique_ptr<Worker> worker;
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
