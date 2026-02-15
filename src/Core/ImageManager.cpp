#include "ImageManager.hpp"
#include "ImageManager.hpp"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

#include "Logging.hpp"

namespace Acheron {
namespace Core {

ImageManager::ImageManager(QObject *parent) : QObject(parent)
{
    networkManager = new QNetworkAccessManager(this);
}

bool ImageManager::isCached(const QUrl &url, const QSize &size)
{
    return cache.contains({ url, size });
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

QPixmap ImageManager::get(const QUrl &url, const QSize &size)
{
    if (isCached(url, size))
        return *cache.object({ url, size });
    request(url, size);
    return placeholder(size);
}

QPixmap ImageManager::placeholder(const QSize &size)
{
    static QPixmap unscaled(":/resources/placeholder.png");
    QPixmap pixmap;
    const QString key = QString("placeholder:%1x%2").arg(size.width()).arg(size.height());
    if (!QPixmapCache::find(key, &pixmap)) {
        pixmap = unscaled.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmapCache::insert(key, pixmap);
    }
    return pixmap;
}

void ImageManager::request(const QUrl &url, const QSize &size)
{
    if (requests.contains({ url, size }))
        return;

    requests.insert({ url, size });

    fetchFromNetwork(url, size);
}

void ImageManager::fetchFromNetwork(const QUrl &url, const QSize &size)
{
    QNetworkRequest request(url);
    QNetworkReply *reply = networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, url, size]() {
        if (reply->error() == QNetworkReply::NoError) {
            QPixmap pixmap;
            if (pixmap.loadFromData(reply->readAll())) {
                pixmap = pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                cache.insert({ url, size }, new QPixmap(pixmap));
                requests.remove({ url, size });
                emit imageFetched(url, size, pixmap);
            } else {
                requests.remove({ url, size });
            }
        } else {
            requests.remove({ url, size });
        }
        reply->deleteLater();
    });
}

} // namespace Core
} // namespace Acheron
