#pragma once

#include <QString>
#include <QUrl>
#include <QCache>
#include <QPixmap>
#include <qlabel.h>

class QNetworkAccessManager;

namespace Acheron {
namespace Core {

struct ImageRequestKey
{
    QUrl url;
    QSize size;
    // QString extension;

    bool operator==(const ImageRequestKey &other) const
    {
        return url == other.url && size == other.size;
    }
};

class ImageManager : public QObject
{
    Q_OBJECT
public:
    explicit ImageManager(QObject *parent = nullptr);

    [[nodiscard]] ImageRequestKey key(const QUrl &url, const QSize &size);

    [[nodiscard]] bool isCached(const QUrl &url, const QSize &size);
    void assign(QLabel *label, const QUrl &url, const QSize &size);
    QPixmap get(const QUrl &url, const QSize &size);
    [[nodiscard]] QPixmap placeholder(const QSize &size);

signals:
    void imageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);

private:
    void request(const QUrl &url, const QSize &size);
    void fetchFromNetwork(const QUrl &url, const QSize &size);

    QNetworkAccessManager *networkManager;

    QSet<ImageRequestKey> requests;
    QCache<ImageRequestKey, QPixmap> cache;
};

} // namespace Core
} // namespace Acheron

namespace std {
template <>
struct hash<Acheron::Core::ImageRequestKey>
{
    size_t operator()(const Acheron::Core::ImageRequestKey &key, size_t seed = 0) const
    {
        return qHashMulti(seed, key.url, key.size);
    }
};
} // namespace std
