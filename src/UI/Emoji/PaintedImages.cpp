#include "UI/Emoji/PaintedImages.hpp"

#include <QWidget>

namespace Acheron {
namespace UI {

PaintedImages::PaintedImages(QWidget *paintedWidget, int budgetKiB) : QObject(paintedWidget), widget(paintedWidget), loaded(budgetKiB) {}

void PaintedImages::setSource(Core::ImageManager *manager, Core::Snowflake account)
{
    this->account = account;
    if (imageManager == manager)
        return;

    QObject::disconnect(fetchedConnection);
    QObject::disconnect(unavailableConnection);
    imageManager = manager;
    clear();
    if (!imageManager)
        return;

    fetchedConnection = connect(imageManager, &Core::ImageManager::imageFetched, this, &PaintedImages::onImageFetched);
    unavailableConnection = connect(imageManager, &Core::ImageManager::imageUnavailable, this, &PaintedImages::onImageUnavailable);
}

QPixmap PaintedImages::pixmapWithoutFetching(const QUrl &url, const QSize &size)
{
    if (!imageManager || !url.isValid())
        return {};

    const Core::ImageRequestKey key{ url, size };
    if (const QPixmap *remembered = loaded.object(key))
        return *remembered;
    if (awaited.contains(key) || imageManager->isUnavailable(key) || !imageManager->isCached(url, size))
        return {};

    const QPixmap cached = imageManager->get(url, size, account);
    remember(key, cached);
    return cached;
}

QPixmap PaintedImages::pixmap(const QUrl &url, const QSize &size, const QRect &paintedRect)
{
    const QPixmap alreadyHere = pixmapWithoutFetching(url, size);
    if (!alreadyHere.isNull() || !imageManager || !url.isValid())
        return alreadyHere;

    const Core::ImageRequestKey key{ url, size };
    const auto awaitedIt = awaited.find(key);
    if (awaitedIt != awaited.end()) {
        *awaitedIt += paintedRect;
        return {};
    }

    if (imageManager->isUnavailable(key))
        return {};

    imageManager->get(url, size, account);
    awaited.insert(key, paintedRect);
    return {};
}

void PaintedImages::remember(const Core::ImageRequestKey &key, const QPixmap &pixmap)
{
    const qint64 bytes = qint64(pixmap.width()) * pixmap.height() * 4;
    loaded.insert(key, new QPixmap(pixmap), int(qBound<qint64>(1, bytes / 1024, loaded.maxCost())));
}

void PaintedImages::clear()
{
    loaded.clear();
    awaited.clear();
}

void PaintedImages::onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap)
{
    const Core::ImageRequestKey key{ url, size };
    const auto it = awaited.constFind(key);
    if (it == awaited.constEnd())
        return;

    const QRegion region = it.value();
    awaited.erase(it);
    remember(key, pixmap);
    widget->update(region);
    emit fetchFinished(true);
}

void PaintedImages::onImageUnavailable(const QUrl &url, const QSize &size)
{
    if (awaited.remove({ url, size }))
        emit fetchFinished(false);
}

} // namespace UI
} // namespace Acheron
