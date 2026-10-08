#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace Acheron {
namespace Core {
namespace Media {

class ClipPlayer : public QObject
{
    Q_OBJECT
public:
    using ClipId = quint64;

    explicit ClipPlayer(QObject *parent = nullptr);
    ~ClipPlayer() override;

    [[nodiscard]] static qint64 memoryToPlay(const QSize &nativeSize, const QSize &frameSize);
    void setMemoryLimitForNewClips(qint64 bytes);

    ClipId open(const QString &path, const QSize &boundPixels, Qt::AspectRatioMode fit);
    void close(ClipId id);
    void closeAll();
    void setPlaying(ClipId id, bool playing);
    [[nodiscard]] QImage frame(ClipId id) const;

signals:
    void frameChanged(quint64 id);
    void clipFailed(quint64 id);

private:
    struct Clip;
    using ClipPtr = std::shared_ptr<Clip>;

    void decodeLoop();
    [[nodiscard]] ClipPtr nextClipAwaitingFrame();
    [[nodiscard]] static std::optional<qint64> decodeNextFrame(Clip &clip, QImage &buffer);
    void requestPresent();
    void presentDueFrames();

    QHash<ClipId, ClipPtr> clips;
    ClipId lastId = 0;
    qint64 newClipMemoryLimitBytes = std::numeric_limits<qint64>::max();
    QTimer presentTimer;
    QElapsedTimer clock;

    std::thread decodeThread;
    std::mutex mutex;
    std::condition_variable frameWanted;
    std::vector<ClipPtr> decodeOrder;
    size_t nextToServe = 0;
    bool stopping = false;
    std::atomic<bool> presentRequested{ false };
};

} // namespace Media
} // namespace Core
} // namespace Acheron
