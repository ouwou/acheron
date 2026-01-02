#pragma once

#include <QString>
#include <QUrl>
#include <QCache>
#include <QPixmap>

class QNetworkAccessManager;

namespace Acheron {
namespace Core {

struct ImageRequestKey
{
    QUrl url;
    int size;
    QString extension;

    bool operator==(const ImageRequestKey &other) const
    {
        return url == other.url && size == other.size && extension == other.extension;
    }
};

class ImageManager : public QObject
{
    Q_OBJECT
public:
    explicit ImageManager(QObject *parent = nullptr);

    [[nodiscard]] bool isCached(const QUrl &url);
    [[nodiscard]] QPixmap get(const QUrl &url);
    [[nodiscard]] QPixmap placeholder();
    void request(const QUrl &url);

signals:
    void imageFetched(const QUrl &url, const QPixmap &pixmap);

private:
    void fetchFromNetwork(const QUrl &url);

    QNetworkAccessManager *networkManager;

    QSet<QUrl> requests;
    QCache<QUrl, QPixmap> cache;
};

} // namespace Core
} // namespace Acheron

namespace std {
template <>
struct hash<Acheron::Core::ImageRequestKey>
{
    size_t operator()(const Acheron::Core::ImageRequestKey &key, size_t seed = 0) const
    {
        return qHashMulti(seed, key.url, key.size, key.extension);
    }
};
} // namespace std
