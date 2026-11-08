#include "StreamPreviews.hpp"

#include <QBuffer>
#include <QDateTime>
#include <QPointer>
#include <QSettings>

namespace Acheron {
namespace Core {

StreamPreviews::StreamPreviews(Fetcher fetcher, QObject *parent, Clock clock)
    : QObject(parent), fetcher(std::move(fetcher)), clock(clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); })
{
}

QUrl StreamPreviews::request(const QString &streamKey)
{
    const qint64 now = clock();
    Entry &entry = entries[streamKey];

    if (!entry.url.isEmpty() && now - entry.fetchedAtMs < FRESH_FOR_MS)
        return entry.url;

    if (entry.fetching || now < entry.retryAtMs)
        return QUrl();

    entry.fetching = true;
    fetcher(streamKey, [self = QPointer<StreamPreviews>(this), streamKey](const Discord::StreamPreviewLookup &lookup) {
        if (self)
            self->onLookupDone(streamKey, lookup);
    });
    return QUrl();
}

void StreamPreviews::onLookupDone(const QString &streamKey, const Discord::StreamPreviewLookup &lookup)
{
    auto it = entries.find(streamKey);
    if (it == entries.end())
        return;

    const qint64 now = clock();
    it->fetching = false;
    it->url = lookup.url;

    if (lookup.url.isEmpty()) {
        it->failures++;
        it->retryAtMs = now + lookup.retryAfterMs.value_or(BACKOFF_STEP_MS * it->failures);
    } else {
        it->failures = 0;
        it->retryAtMs = 0;
        it->fetchedAtMs = now;
    }

    emit previewChanged(streamKey, lookup.url);
}

void StreamPreviews::forgetOwner(Snowflake ownerId)
{
    for (auto it = entries.begin(); it != entries.end();) {
        const auto key = Discord::StreamKey::parse(it.key());
        if (key && key->ownerId == ownerId)
            it = entries.erase(it);
        else
            ++it;
    }
}

void StreamPreviews::clear()
{
    entries.clear();
}

StreamPreviewUploader::StreamPreviewUploader(Upload upload, QObject *parent)
    : QObject(parent), upload(std::move(upload))
{
    due.setSingleShot(true);
    connect(&due, &QTimer::timeout, this, &StreamPreviewUploader::onDue);
}

bool StreamPreviewUploader::enabled()
{
    return QSettings().value(QStringLiteral("video/stream_previews"), true).toBool();
}

void StreamPreviewUploader::setEnabled(bool enabled)
{
    QSettings().setValue(QStringLiteral("video/stream_previews"), enabled);
}

QString StreamPreviewUploader::thumbnailOf(const QImage &picture)
{
    if (picture.isNull())
        return {};

    const QImage fitted = picture.scaled(LARGEST, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    if (!fitted.save(&buffer, "JPEG", JPEG_QUALITY))
        return {};
    return QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(jpeg.toBase64());
}

void StreamPreviewUploader::start(const QString &key)
{
    stop();
    if (!enabled())
        return;
    streamKey = key;
    due.start(FIRST_WAIT_MS);
}

void StreamPreviewUploader::stop()
{
    run++;
    due.stop();
    pictureWanted = false;
    streamKey.clear();
}

void StreamPreviewUploader::setStalled(bool value)
{
    stalled = value;
}

void StreamPreviewUploader::onDue()
{
    if (stalled)
        due.start(RETRY_WAIT_MS);
    else
        pictureWanted = true;
}

void StreamPreviewUploader::offer(const QImage &picture)
{
    if (!pictureWanted)
        return;
    pictureWanted = false;

    const QString thumbnail = thumbnailOf(picture);
    if (thumbnail.isEmpty()) {
        due.start(RETRY_WAIT_MS);
        return;
    }

    upload(streamKey, thumbnail, [self = QPointer<StreamPreviewUploader>(this), startedBy = run](bool uploaded) {
        if (self)
            self->onUploadDone(startedBy, uploaded);
    });
}

void StreamPreviewUploader::onUploadDone(quint64 startedBy, bool uploaded)
{
    if (startedBy == run)
        due.start(uploaded ? UPLOADED_WAIT_MS : RETRY_WAIT_MS);
}

} // namespace Core
} // namespace Acheron
