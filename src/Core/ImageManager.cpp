#include "ImageManager.hpp"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QCryptographicHash>
#include <QFile>
#include <QUrlQuery>
#include <QApplication>

#include "Logging.hpp"

namespace Acheron {
namespace Core {

ImageManager::ImageManager(QObject *parent) : QObject(parent)
{
    networkManager = new QNetworkAccessManager(this);
    cache.setMaxCost(300);

    if (!tempDir.isValid())
        qCWarning(LogCore) << "Failed to create temp directory for image cache";
}

bool ImageManager::isCached(const QUrl &url, const QSize &size)
{
    ImageRequestKey k{ url, size };
    if (pinnedImages.contains(k) || cache.contains(k))
        return true;

    QString path = getCachePath(url, size);
    return QFile::exists(path);
}

void ImageManager::assign(QLabel *label, const QUrl &url, const QSize &size)
{
    if (!label)
        return;

    // just in case
    disconnect(this, &ImageManager::imageFetched, label, nullptr);

    QPixmap pixmap = get(url, size);
    label->setPixmap(pixmap);

    if (!isCached(url, size)) {
        connect(this, &ImageManager::imageFetched, label,
                [=](const QUrl &u, const QSize &s, const QPixmap &p) {
                    if (u == url && s == size)
                        label->setPixmap(p);
                });
    }
}

QPixmap ImageManager::get(const QUrl &url, const QSize &size, PinGroup pin)
{
    return getImpl(url, size, pin, true);
}

QPixmap ImageManager::getIfCached(const QUrl &url, const QSize &size, PinGroup pin)
{
    return getImpl(url, size, pin, false);
}

QPixmap ImageManager::getImpl(const QUrl &url, const QSize &size, PinGroup pin, bool fetchIfNeeded)
{
    ImageRequestKey k{ url, size };

    auto pinnedIt = pinnedImages.constFind(k);
    if (pinnedIt != pinnedImages.constEnd()) {
        if (pin != PinGroup::None && !pinGroupKeys.contains(pin, k))
            pinGroupKeys.insert(pin, k);
        return pinnedIt.value();
    }

    if (cache.contains(k)) {
        QPixmap pixmap = *cache.object(k);
        if (pin != PinGroup::None) {
            pinnedImages.insert(k, pixmap);
            pinGroupKeys.insert(pin, k);
            cache.remove(k);
        }
        return pixmap;
    }

    // check disk cache
    QString path = getCachePath(url, size);
    if (QFile::exists(path)) {
        QPixmap pixmap;
        if (pixmap.load(path)) {
            qreal dpr = qApp->devicePixelRatio();
            bool proxy = isDiscordProxyUrl(url);

            if (proxy) {
                QSize physicalSize(qRound(size.width() * dpr), qRound(size.height() * dpr));
                if (pixmap.size() != physicalSize)
                    pixmap = pixmap.scaled(physicalSize, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
                pixmap.setDevicePixelRatio(dpr);
            } else {
                if (pixmap.size() != size)
                    pixmap = pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }

            if (pin != PinGroup::None) {
                pinnedImages.insert(k, pixmap);
                pinGroupKeys.insert(pin, k);
            } else {
                cache.insert(k, new QPixmap(pixmap));
            }
            return pixmap;
        }
    }

    if (fetchIfNeeded) {
        request(url, size, pin);
    }

    return placeholder(size);
}

QPixmap ImageManager::placeholder(const QSize &size)
{
    qreal dpr = qApp->devicePixelRatio();
    QSize physicalSize(qRound(size.width() * dpr), qRound(size.height() * dpr));
    QPixmap pixmap(physicalSize);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(QColor(60, 60, 60));
    return pixmap;
}

void ImageManager::request(const QUrl &url, const QSize &size, PinGroup pin)
{
    ImageRequestKey k{ url, size };
    if (requests.contains(k)) {
        // promote
        if (pin != PinGroup::None) {
            auto it = pendingPins.find(k);
            if (it == pendingPins.end() || it.value() == PinGroup::None)
                pendingPins.insert(k, pin);
        }
        return;
    }

    requests.insert(k);
    if (pin != PinGroup::None)
        pendingPins.insert(k, pin);

    fetchFromNetwork(url, size, pin);
}

void ImageManager::fetchFromNetwork(const QUrl &url, const QSize &size, PinGroup pin)
{
    qreal dpr = qApp->devicePixelRatio();
    bool proxy = isDiscordProxyUrl(url);

    QUrl fetchUrl = proxy ? buildOptimizedUrl(url, size, dpr) : url;
    QNetworkRequest request(fetchUrl);
    QNetworkReply *reply = networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, url, size, proxy, dpr]() {
        ImageRequestKey k{ url, size };
        PinGroup pin = pendingPins.value(k, PinGroup::None);
        pendingPins.remove(k);

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(LogCore) << "Failed to fetch image:" << reply->errorString();
            requests.remove(k);
            reply->deleteLater();
            return;
        }

        QByteArray data = reply->readAll();
        reply->deleteLater();

        // save to disk cache
        QString path = getCachePath(url, size);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(data);
            file.close();
        }

        QPixmap pixmap;
        if (pixmap.loadFromData(data)) {
            if (proxy) {
                QSize physicalSize(qRound(size.width() * dpr), qRound(size.height() * dpr));
                if (pixmap.size() != physicalSize)
                    pixmap = pixmap.scaled(physicalSize, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
                pixmap.setDevicePixelRatio(dpr);
            } else {
                pixmap = pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }

            if (pin != PinGroup::None) {
                pinnedImages.insert(k, pixmap);
                pinGroupKeys.insert(pin, k);
            } else {
                cache.insert(k, new QPixmap(pixmap));
            }

            requests.remove(k);
            emit imageFetched(url, size, pixmap);
        } else {
            requests.remove(k);
        }
    });
}

void ImageManager::unpinGroup(PinGroup group)
{
    if (group == PinGroup::None)
        return;

    QList<ImageRequestKey> keys = pinGroupKeys.values(group);
    pinGroupKeys.remove(group);

    for (const auto &k : keys) {
        // the same url could be pinned by multiple groups
        bool stillPinned = false;
        for (auto it = pinGroupKeys.cbegin(); it != pinGroupKeys.cend(); ++it) {
            if (it.value() == k) {
                stillPinned = true;
                break;
            }
        }

        if (!stillPinned) {
            auto it = pinnedImages.find(k);
            if (it != pinnedImages.end()) {
                // send it back to the lru
                cache.insert(k, new QPixmap(it.value()));
                pinnedImages.erase(it);
            }
        }
    }
}

QSize ImageManager::calculateDisplaySize(const QSize &original)
{
    if (!original.isValid() || original.isEmpty())
        return QSize(MaxDisplayWidth, MaxDisplayHeight);

    if (original.width() <= MaxDisplayWidth && original.height() <= MaxDisplayHeight)
        return original;

    return original.scaled(MaxDisplayWidth, MaxDisplayHeight, Qt::KeepAspectRatio);
}

QString ImageManager::getCachePath(const QUrl &url, const QSize &size) const
{
    QString compound = url.toString() + QStringLiteral(":%1x%2").arg(size.width()).arg(size.height());
    QByteArray hash =
            QCryptographicHash::hash(compound.toUtf8(), QCryptographicHash::Sha1);
    QString filename = QString::fromLatin1(hash.toHex());
    return tempDir.filePath(filename);
}

bool ImageManager::isDiscordProxyUrl(const QUrl &url)
{
    QString host = url.host();
    return host == u"media.discordapp.net" || host.startsWith(u"images-ext-");
}

QUrl ImageManager::buildOptimizedUrl(const QUrl &proxyUrl, const QSize &displaySize, qreal dpr)
{
    QUrl optimized = proxyUrl;
    QUrlQuery query(optimized);

    query.addQueryItem("format", "webp");
    query.addQueryItem("quality", "lossless");

    if (displaySize.isValid() && !displaySize.isEmpty()) {
        int physicalWidth = qRound(displaySize.width() * dpr);
        int physicalHeight = qRound(displaySize.height() * dpr);
        query.addQueryItem("width", QString::number(physicalWidth));
        query.addQueryItem("height", QString::number(physicalHeight));
    }

    optimized.setQuery(query);
    return optimized;
}

} // namespace Core
} // namespace Acheron
