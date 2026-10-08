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

#include <optional>

#include "Core/ImageManager.hpp"
#include "Core/Media/ClipPlayer.hpp"
#include "Core/MiBRange.hpp"
#include "Core/Snowflake.hpp"

class QAbstractItemModel;

namespace Acheron {
namespace UI {

class ChatView;

enum class ClipFit {
    Fit,
    Crop,
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
    };

    Kind kind;
    Core::Snowflake attachmentOrMessageId;
    int embedIndex = 0;
    int imageIndex = 0;

    bool operator==(const GifKey &other) const
    {
        return kind == other.kind && attachmentOrMessageId == other.attachmentOrMessageId && embedIndex == other.embedIndex && imageIndex == other.imageIndex;
    }
};

inline size_t qHash(const GifKey &key, size_t seed = 0)
{
    return qHashMulti(seed, quint8(key.kind), quint64(key.attachmentOrMessageId), key.embedIndex, key.imageIndex);
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

} // namespace GifKeys

class GifPlayback : public QObject
{
    Q_OBJECT
public:
    static constexpr int FewestPlayingAtOnce = 1;
    static constexpr int DefaultPlayingAtOnce = 8;
    static constexpr int MostPlayingAtOnce = 32;
    static constexpr Core::MiBRange ClipMemoryLimit{ 8, 48, 256 };

    explicit GifPlayback(ChatView *view);

    void setImageManager(Core::ImageManager *manager);
    void setPlayMode(GifPlayMode playMode);
    void setMaxPlayingAtOnce(int count);
    void setClipMemoryLimitMiB(int mib);
    void setViewVisible(bool visible);

    void attachModel(QAbstractItemModel *model);
    void reset();
    void viewportMoved();
    void updateHover(const QPoint &viewportPos);
    void clearHover();

    void setPaintDamage(const QRegion &region) { damage = region; }
    void beginRow(const QModelIndex &index, const QRect &rowRect);
    void endRow();

    struct Shown
    {
        QImage frame;
        bool badge = false;
    };

    [[nodiscard]] Shown show(const GifKey &key, const QUrl &animatedUrl, const QRect &viewportRect, ClipFit fit);

    struct Repaint
    {
        QRect rect;
        QImage frame;
        ClipFit fit;
        bool badge;
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
    [[nodiscard]] QSet<GifKey> chooseWanted(Starts starts) const;
    void scheduleReconcile();
    void reconcile(Starts starts);
    void dropSurfacesOutOfView();
    void play(const GifKey &key);
    void closeClip(const GifKey &key);
    void closeClipsPausedTooLong();
    void repaint(const GifKey &key);
    void repaintAll();
    void reconcileAndRepaint();
    void onDownloadReady(const QUrl &url, const QSize &size);
    void onDownloadFailed(const QUrl &url, const QSize &size);
    void onFrameChanged(quint64 clip);
    void onClipFailed(quint64 clip);

    ChatView *view;
    QPointer<Core::ImageManager> imageManager;
    Core::Media::ClipPlayer *player;
    QPointer<QAbstractItemModel> boundModel;

    QHash<GifKey, Surface> surfaces;
    QHash<GifKey, OpenClip> openClips;
    QHash<ClipId, GifKey> keyOfClip;
    QSet<GifKey> wanted;
    QSet<GifKey> undecided;
    QSet<GifKey> unplayable;
    std::optional<QPoint> cursorPos;
    std::optional<GifKey> hoveredKey;

    QRegion damage;
    std::optional<Recording> recording;

    QTimer startTimer;
    QTimer pruneTimer;
    QTimer pausedTooLongTimer;

    GifPlayMode mode = GifPlayMode::WhenFocused;
    int maxPlayingAtOnce = DefaultPlayingAtOnce;
    int clipMemoryLimitMiB = ClipMemoryLimit.fallback;
    bool focused = true;
    bool viewVisible = false;
};

} // namespace UI
} // namespace Acheron
