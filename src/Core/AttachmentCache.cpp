#include "AttachmentCache.hpp"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QCryptographicHash>
#include <QFile>
#include <QDir>
#include <QUrlQuery>
#include <QApplication>

#include "Logging.hpp"

namespace Acheron {
namespace Core {

namespace {

QUrl buildOptimizedUrl(const QUrl &proxyUrl, const QSize &displaySize, qreal devicePixelRatio)
{
    QUrl optimized = proxyUrl;
    QUrlQuery query(optimized);

    query.addQueryItem("format", "webp");
    query.addQueryItem("quality", "lossless");

    if (displaySize.isValid() && !displaySize.isEmpty()) {
        // request physical pixel dimensions for HiDPI displays
        int physicalWidth = qRound(displaySize.width() * devicePixelRatio);
        int physicalHeight = qRound(displaySize.height() * devicePixelRatio);
        query.addQueryItem("width", QString::number(physicalWidth));
        query.addQueryItem("height", QString::number(physicalHeight));
    }

    optimized.setQuery(query);
    return optimized;
}

} // anonymous namespace

AttachmentCache::AttachmentCache(QObject *parent) : QObject(parent)
{
    networkManager = new QNetworkAccessManager(this);

    if (!tempDir.isValid())
        qCWarning(LogCore) << "Failed to create temp directory for attachment cache";
}

QSize AttachmentCache::calculateDisplaySize(const QSize &original)
{
    if (!original.isValid() || original.isEmpty())
        return QSize(MaxWidth, MaxHeight);

    if (original.width() <= MaxWidth && original.height() <= MaxHeight)
        return original;

    return original.scaled(MaxWidth, MaxHeight, Qt::KeepAspectRatio);
}

bool AttachmentCache::isCached(const QUrl &proxyUrl) const
{
    if (memoryCache.contains(proxyUrl))
        return true;

    QString path = getCachePath(proxyUrl);
    return QFile::exists(path);
}

QPixmap AttachmentCache::get(const QUrl &proxyUrl, const QSize &originalSize)
{
    QSize displaySize = calculateDisplaySize(originalSize);
    qreal dpr = qApp->devicePixelRatio();

    // check memory cache first
    if (memoryCache.contains(proxyUrl))
        return memoryCache.value(proxyUrl);

    // check disk cache
    QString path = getCachePath(proxyUrl);
    if (QFile::exists(path)) {
        QPixmap pixmap;
        if (pixmap.load(path)) {
            // scale to physical pixels then set device pixel ratio
            QSize physicalSize(qRound(displaySize.width() * dpr),
                               qRound(displaySize.height() * dpr));
            if (pixmap.size() != physicalSize)
                pixmap = pixmap.scaled(physicalSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            pixmap.setDevicePixelRatio(dpr);
            memoryCache.insert(proxyUrl, pixmap);
            return pixmap;
        }
    }

    // need to fetch from network
    if (!pendingRequests.contains(proxyUrl)) {
        pendingRequests.insert(proxyUrl);
        fetchFromNetwork(proxyUrl, displaySize);
    }

    return placeholder(displaySize);
}

QString AttachmentCache::getCachePath(const QUrl &url) const
{
    QByteArray hash = QCryptographicHash::hash(url.toString().toUtf8(), QCryptographicHash::Sha1);
    QString filename = QString::fromLatin1(hash.toHex());
    return tempDir.filePath(filename);
}

QPixmap AttachmentCache::placeholder(const QSize &size)
{
    qreal dpr = qApp->devicePixelRatio();
    QSize physicalSize(qRound(size.width() * dpr), qRound(size.height() * dpr));
    QPixmap pixmap(physicalSize);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(QColor(60, 60, 60));
    return pixmap;
}

void AttachmentCache::fetchFromNetwork(const QUrl &url, const QSize &displaySize)
{
    qreal dpr = qApp->devicePixelRatio();

    // request optimized size from Discord's image proxy (in physical pixels)
    QUrl fetchUrl = buildOptimizedUrl(url, displaySize, dpr);
    QNetworkRequest request(fetchUrl);
    QNetworkReply *reply = networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, url, displaySize, dpr]() {
        pendingRequests.remove(url);

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(LogCore) << "Failed to fetch attachment:" << reply->errorString();
            reply->deleteLater();
            return;
        }

        QByteArray data = reply->readAll();
        reply->deleteLater();

        // save to disk cache (keyed by original url)
        QString path = getCachePath(url);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(data);
            file.close();
        }

        // load pixmap - already sized by server in physical pixels
        QPixmap pixmap;
        if (pixmap.loadFromData(data)) {
            QSize physicalSize(qRound(displaySize.width() * dpr),
                               qRound(displaySize.height() * dpr));
            if (pixmap.size() != physicalSize)
                pixmap = pixmap.scaled(physicalSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            pixmap.setDevicePixelRatio(dpr);
            memoryCache.insert(url, pixmap);
            emit attachmentFetched(url, pixmap);
        }
    });
}

} // namespace Core
} // namespace Acheron
