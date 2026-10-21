#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <memory>
#include <mutex>

#include "Core/Media/VideoDecoder.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Video/TileVisibility.hpp"
#include "Discord/Voice/SinkWants.hpp"
#include "Discord/Voice/VoiceClient.hpp"

namespace Acheron {
namespace Core {

namespace Audio {
class VoiceConnection;
}

class CallVideo : public QObject
{
    Q_OBJECT
public:
    explicit CallVideo(QObject *parent = nullptr);
    ~CallVideo() override;

    // call before the connection starts, and detach() once it has stopped
    void attach(Audio::VoiceConnection *connection, Snowflake ownUserId);
    void detach();

    [[nodiscard]] QList<Snowflake> senders() const;

    void setTile(Snowflake userId, std::shared_ptr<Media::VideoFrameSink> sink, int pixelCount, bool visible);
    void removeTile(Snowflake userId);

    void setFocused(Snowflake userId);

    [[nodiscard]] bool isVideoDisabled(Snowflake userId) const;
    void setVideoDisabled(Snowflake userId, bool disabled);

    [[nodiscard]] Media::VideoDecoder::Stats decoderStats(Snowflake userId) const;

    [[nodiscard]] static int maxDecoders();
    static void setMaxDecoders(int decoders);

    static constexpr int REPLAN_DELAY_MS = 100;
    static constexpr int DEFAULT_MAX_DECODERS = 9;

signals:
    void sendersChanged();
    void videoDisabledChanged(Snowflake userId);

private slots:
    void onLayersChanged(Snowflake userId, const QList<Discord::Voice::VideoLayer> &layers);
    void onUserReachedServer(Snowflake userId);
    void onUserLeftServer(Snowflake userId);
    void replan();

private:
    struct Tile
    {
        std::shared_ptr<Media::VideoFrameSink> sink;
        int pixels = 0;
    };

    void scheduleReplan();
    [[nodiscard]] std::shared_ptr<Media::VideoDecoder> decoderOf(Snowflake userId) const;

    Audio::VoiceConnection *connection = nullptr;
    QHash<Snowflake, QList<Discord::Voice::VideoLayer>> layersByUser;
    QHash<Snowflake, Tile> tiles;
    TileVisibility visibility;
    QSet<Snowflake> disabledUsers;
    Snowflake focusedUser;
    Snowflake ownUser;
    // with exactly one, the official client asks for no more than the tile needs, except in stage channels,
    // which this client doesnt tell apart
    QSet<Snowflake> othersOnServer;

    QElapsedTimer clock;
    QTimer replanTimer;
    QTimer expiryTimer;

    // read on the connection's thread for every frame
    mutable std::mutex decoderMutex;
    QHash<Snowflake, std::shared_ptr<Media::VideoDecoder>> decoders;
};

} // namespace Core
} // namespace Acheron
