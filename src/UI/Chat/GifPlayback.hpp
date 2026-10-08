#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QModelIndex>
#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QRect>
#include <QRegion>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>

#include "Core/Gifs/FavoriteGifs.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Media/ClipPlayer.hpp"
#include "Core/MiBRange.hpp"
#include "Core/Snowflake.hpp"

class QAbstractItemModel;
class QAbstractItemView;

namespace Acheron {
namespace UI {

enum class ClipFit {
    Fit,
    Crop,
};

enum class GifStar {
    Hidden,
    Outline,
    Filled,
};

enum class GifPlayMode {
    WhenFocused,
    OnHover,
    Never,
};

[[nodiscard]] GifPlayMode gifPlayModeFromSetting(const QString &value);

struct GifKey
{
    enum class Kind : quint8 {
        Attachment,
        EmbedVideo,
        EmbedThumbnail,
        EmbedImage,
        FavoriteTile,
    };

    Kind kind;
    Core::Snowflake attachmentOrMessageId;
    int embedOrTileIndex = 0;
    int imageIndex = 0;

    bool operator==(const GifKey &other) const
    {
        return kind == other.kind && attachmentOrMessageId == other.attachmentOrMessageId && embedOrTileIndex == other.embedOrTileIndex && imageIndex == other.imageIndex;
    }
};

inline size_t qHash(const GifKey &key, size_t seed = 0)
{
    return qHashMulti(seed, quint8(key.kind), quint64(key.attachmentOrMessageId), key.embedOrTileIndex, key.imageIndex);
}

namespace GifKeys {

inline GifKey attachment(Core::Snowflake attachmentId)
{
    return { GifKey::Kind::Attachment, attachmentId };
}

inline GifKey embedVideo(Core::Snowflake messageId, int embedIndex)
{
    return { GifKey::Kind::EmbedVideo, messageId, embedIndex };
}

inline GifKey embedThumbnail(Core::Snowflake messageId, int embedIndex)
{
    return { GifKey::Kind::EmbedThumbnail, messageId, embedIndex };
}

inline GifKey embedImage(Core::Snowflake messageId, int embedIndex, int imageIndex)
{
    return { GifKey::Kind::EmbedImage, messageId, embedIndex, imageIndex };
}

inline GifKey favoriteTile(int tileIndex)
{
    return { GifKey::Kind::FavoriteTile, Core::Snowflake(0), tileIndex };
}

} // namespace GifKeys

class GifPlayback : public QObject
{
    Q_OBJECT
public:
    static constexpr int FewestPlayingAtOnce = 1;
    static constexpr int DefaultPlayingAtOnce = 8;
    static constexpr int MostPlayingAtOnce = 32;
    static constexpr Core::MiBRange ClipMemoryLimit{ 8, 48, 256 };

    using AccountSource = std::function<Core::Snowflake()>;

    GifPlayback(QAbstractItemView *view, AccountSource playingAccount);

    void setImageManager(Core::ImageManager *manager);
    void setFavorites(Core::FavoriteGifs *favoriteGifs);
    void setPlayMode(GifPlayMode playMode);
    void setMaxPlayingAtOnce(int count);
    void setClipMemoryLimitMiB(int mib);
    void setViewVisible(bool visible);

    void attachModel(QAbstractItemModel *model);
    void reset();
    void viewportMoved();
    void updateHover(const QPoint &viewportPos);
    void clearHover();
    [[nodiscard]] bool isOverStar(const QPoint &viewportPos) const;
    bool toggleFavoriteAt(const QPoint &viewportPos);

    void setPaintDamage(const QRegion &region) { damage = region; }
    void beginRow(const QModelIndex &index, const QRect &rowRect);
    void endRow();

    struct Shown
    {
        QImage frame;
        bool badge = false;
        GifStar star = GifStar::Hidden;
    };

    [[nodiscard]] Shown show(const GifKey &key, const QUrl &animatedUrl, const QRect &viewportRect, ClipFit fit, const Core::FavoriteGifCandidate &favorite = {});

    struct Repaint
    {
        QRect rect;
        QImage frame;
        ClipFit fit;
        bool badge;
        GifStar star;
    };

    [[nodiscard]] std::optional<QList<Repaint>> clipOnlyRepaint(const QModelIndex &index, const QRect &rowRect) const;

private:
    using ClipId = Core::Media::ClipPlayer::ClipId;

    struct Surface
    {
        QPersistentModelIndex index;
        QRect rectInRow;
        QUrl url;
        ClipFit fit = ClipFit::Fit;
        Core::FavoriteGifCandidate favorite;
    };

    struct OpenClip
    {
        ClipId id;
        QUrl url;
        QSize boundPixels;
        ClipFit fit;
    };

    struct Recording
    {
        QModelIndex index;
        QPoint origin;
        QSet<GifKey> keys;
    };

    enum class Starts {
        None,
        Allowed,
    };

    [[nodiscard]] bool gateOpen() const;
    [[nodiscard]] bool expectedToPlay(const GifKey &key) const;
    [[nodiscard]] QRect viewportRect(const Surface &surface) const;
    [[nodiscard]] std::optional<GifKey> keyUnderCursor() const;
    [[nodiscard]] std::optional<GifKey> keyAt(const QPoint &viewportPos) const;
    [[nodiscard]] GifStar starFor(const GifKey &key, const Surface &surface) const;
    [[nodiscard]] const Surface *surfaceWithStarAt(const QPoint &viewportPos) const;
    [[nodiscard]] QSet<GifKey> chooseWanted(Starts starts) const;
    void scheduleReconcile();
    void reconcile(Starts starts);
    void dropSurfacesOutOfView();
    void play(const GifKey &key);
    void download(const GifKey &key, const QUrl &url);
    void holdUntilRetry(const GifKey &key);
    void closeClip(const GifKey &key);
    void closeClipsPausedTooLong();
    void repaint(const GifKey &key);
    void repaintAll();
    void reconcileAndRepaint();
    void onDownloadReady(const QUrl &url, const QSize &size);
    void onDownloadFailed(const QUrl &url, const QSize &size);
    void forgetFailuresOf(const GifKey &key);
    void onFrameChanged(quint64 clip);
    void onClipFailed(quint64 clip);

    QAbstractItemView *view;
    AccountSource playingAccount;
    QPointer<Core::ImageManager> imageManager;
    QPointer<Core::FavoriteGifs> favorites;
    Core::Media::ClipPlayer *player;
    QPointer<QAbstractItemModel> boundModel;

    QHash<GifKey, Surface> surfaces;
    QHash<GifKey, OpenClip> openClips;
    QHash<ClipId, GifKey> keyOfClip;
    QSet<GifKey> wanted;
    QSet<GifKey> undecided;
    QSet<GifKey> unplayable;
    QSet<GifKey> downloadsOnHold;
    QSet<QUrl> downloadsInFlight;
    std::optional<QPoint> cursorPos;
    std::optional<GifKey> hoveredKey;

    QRegion damage;
    std::optional<Recording> recording;

    QTimer startTimer;
    QTimer pruneTimer;
    QTimer pausedTooLongTimer;
    QTimer retryDownloadsTimer;

    GifPlayMode mode = GifPlayMode::WhenFocused;
    int maxPlayingAtOnce = DefaultPlayingAtOnce;
    int clipMemoryLimitMiB = ClipMemoryLimit.fallback;
    bool focused = true;
    bool viewVisible = false;
};

} // namespace UI
} // namespace Acheron
