#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>

#include "Core/Snowflake.hpp"
#include "Discord/StreamKey.hpp"

namespace Acheron {
namespace Core {

// cached as the official client does: a result lasts two minutes, a failed lookup backs off ten seconds
// longer each time, and a rate limit waits as long as the server asks
class StreamPreviews : public QObject
{
    Q_OBJECT
public:
    using Fetcher = std::function<void(const QString &streamKey, std::function<void(const Discord::StreamPreviewLookup &)> done)>;
    using Clock = std::function<qint64()>;

    explicit StreamPreviews(Fetcher fetcher, QObject *parent = nullptr, Clock clock = {});

    QUrl request(const QString &streamKey);

    void forgetOwner(Snowflake ownerId);
    void clear();

    static constexpr qint64 FRESH_FOR_MS = 2 * 60 * 1000;
    static constexpr qint64 BACKOFF_STEP_MS = 10 * 1000;

signals:
    void previewChanged(const QString &streamKey, const QUrl &url);

private:
    struct Entry
    {
        QUrl url;
        qint64 fetchedAtMs = 0;
        qint64 retryAtMs = 0;
        int failures = 0;
        bool fetching = false;
    };

    void onLookupDone(const QString &streamKey, const Discord::StreamPreviewLookup &lookup);

    Fetcher fetcher;
    Clock clock;
    QHash<QString, Entry> entries;
};

// uploaded when the official client does: half a second after the video starts, every five minutes,
// and a minute after a failure or a due upload while capture was stalled
class StreamPreviewUploader : public QObject
{
    Q_OBJECT
public:
    using Upload = std::function<void(const QString &streamKey, const QString &thumbnail, std::function<void(bool uploaded)> done)>;

    explicit StreamPreviewUploader(Upload upload, QObject *parent = nullptr);

    [[nodiscard]] static bool enabled();
    static void setEnabled(bool enabled);
    [[nodiscard]] static QString thumbnailOf(const QImage &picture);

    void start(const QString &streamKey);
    void stop();
    void setStalled(bool stalled);
    void offer(const QImage &picture);

    [[nodiscard]] std::optional<int> waitMs() const { return due.isActive() ? std::optional<int>(due.interval()) : std::nullopt; }

    static constexpr int FIRST_WAIT_MS = 500;
    static constexpr int UPLOADED_WAIT_MS = 5 * 60 * 1000;
    static constexpr int RETRY_WAIT_MS = 60 * 1000;
    static constexpr QSize LARGEST = QSize(512, 288);
    // a browser canvas's default JPEG quality, which the official client's thumbnails use
    static constexpr int JPEG_QUALITY = 92;

private slots:
    void onDue();

private:
    void onUploadDone(quint64 startedBy, bool uploaded);

    Upload upload;
    QString streamKey;
    QTimer due;
    bool stalled = false;
    bool pictureWanted = false;
    quint64 run = 0;
};

} // namespace Core
} // namespace Acheron
