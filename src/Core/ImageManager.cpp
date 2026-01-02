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

bool ImageManager::isCached(const QUrl &url)
{
    return cache.contains(url);
}

QPixmap ImageManager::get(const QUrl &url)
{
    return *cache.object(url);
}

QPixmap ImageManager::placeholder()
{
    static QPixmap pixmap(":/resources/placeholder.png");
    return pixmap;
}

void ImageManager::request(const QUrl &url)
{
    if (requests.contains(url))
        return;

    requests.insert(url);

    fetchFromNetwork(url);
}

void ImageManager::fetchFromNetwork(const QUrl &url)
{
    QNetworkRequest request(url);
    QNetworkReply *reply = networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [=]() {
        if (reply->error() == QNetworkReply::NoError) {
            QPixmap pixmap;
            if (pixmap.loadFromData(reply->readAll())) {
                cache.insert(url, new QPixmap(pixmap));
                requests.remove(url);
                emit imageFetched(url, pixmap);
            }
        }
        reply->deleteLater();
    });
}

} // namespace Core
} // namespace Acheron

size_t qHash(const Acheron::Core::ImageRequestKey &key, size_t seed)
{
    return qHashMulti(seed, key.url, key.size, key.extension);
}
