#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QCache>
#include <QPixmap>
#include <QTemporaryDir>
#include <QHash>
#include <QSet>
#include <qlabel.h>

class QNetworkAccessManager;

namespace Acheron {
namespace Core {

enum class PinGroup {
    None,
    ChannelList,
    ChatView,
};

struct ImageRequestKey
{
    QUrl url;
    QSize size;

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

    static constexpr int MaxDisplayWidth = 400;
    static constexpr int MaxDisplayHeight = 300;

    [[nodiscard]] bool isCached(const QUrl &url, const QSize &size);
    void assign(QLabel *label, const QUrl &url, const QSize &size);
    QPixmap get(const QUrl &url, const QSize &size, PinGroup pin = PinGroup::None);
    QPixmap getIfCached(const QUrl &url, const QSize &size, PinGroup pin = PinGroup::None);
    [[nodiscard]] QPixmap placeholder(const QSize &size);

    void unpinGroup(PinGroup group);

    [[nodiscard]] static QSize calculateDisplaySize(const QSize &original);

signals:
    void imageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);

private:
    QPixmap getImpl(const QUrl &url, const QSize &size, PinGroup pin, bool fetchIfNeeded);
    void request(const QUrl &url, const QSize &size, PinGroup pin);
    void fetchFromNetwork(const QUrl &url, const QSize &size, PinGroup pin);
    QString getCachePath(const QUrl &url, const QSize &size) const;
    static bool isDiscordProxyUrl(const QUrl &url);
    static QUrl buildOptimizedUrl(const QUrl &proxyUrl, const QSize &displaySize, qreal dpr);

    QNetworkAccessManager *networkManager;
    QTemporaryDir tempDir;

    QSet<ImageRequestKey> requests;
    QHash<ImageRequestKey, PinGroup> pendingPins;
    QCache<ImageRequestKey, QPixmap> cache;
    QHash<ImageRequestKey, QPixmap> pinnedImages;
    QMultiHash<PinGroup, ImageRequestKey> pinGroupKeys;
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
