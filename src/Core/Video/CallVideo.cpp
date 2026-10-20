#include "CallVideo.hpp"

#include "Core/Audio/VoiceConnection.hpp"

#include <QSettings>

#include <algorithm>

namespace Acheron {
namespace Core {

using Discord::Voice::VoiceClient;

namespace {

const char *const DISABLED_USERS_KEY = "video/disabled_users";
const char *const MAX_DECODERS_KEY = "video/max_decoders";

// Handing a picture over on the graphics card takes the same time whatever its size, and for a
// small one that is more than decoding it in memory and sending it up: with nine tiles on a
// Radeon RX 7800 XT, about 1.0 ms of processor time a picture against 0.6 ms for one of
// 640x360, before either is decoded. So a picture asked for at no more than this, which is
// 640x360 on the ladder and what everyone is asked for from four cameras up, is decoded in
// memory. A decoder stays as it was made when what is asked of its sender changes later.
constexpr int MOST_ASKED_OF_A_PICTURE_DECODED_IN_MEMORY = 70;

} // namespace

CallVideo::CallVideo(QObject *parent)
    : QObject(parent)
{
    clock.start();

    const QStringList stored = QSettings().value(DISABLED_USERS_KEY).toStringList();
    for (const QString &id : stored)
        disabledUsers.insert(Snowflake(id.toULongLong()));

    replanTimer.setSingleShot(true);
    replanTimer.setInterval(REPLAN_DELAY_MS);
    connect(&replanTimer, &QTimer::timeout, this, &CallVideo::replan);

    // Hidden tiles lose their video as time passes, with nothing else to prompt a new plan.
    expiryTimer.setInterval(1000);
    connect(&expiryTimer, &QTimer::timeout, this, &CallVideo::replan);
}

CallVideo::~CallVideo()
{
    detach();
}

int CallVideo::maxDecoders()
{
    return std::max(1, QSettings().value(MAX_DECODERS_KEY, DEFAULT_MAX_DECODERS).toInt());
}

void CallVideo::setMaxDecoders(int decoders)
{
    QSettings().setValue(MAX_DECODERS_KEY, std::max(1, decoders));
}

void CallVideo::attach(Audio::VoiceConnection *voiceConnection, Snowflake ownUserId)
{
    detach();
    connection = voiceConnection;
    ownUser = ownUserId;

    connect(connection, &Audio::VoiceConnection::remoteVideoLayersChanged, this, &CallVideo::onLayersChanged);
    connect(connection, &Audio::VoiceConnection::clientReachedServer, this, &CallVideo::onUserReachedServer);
    connect(connection, &Audio::VoiceConnection::clientDisconnected, this, &CallVideo::onUserLeftServer);
    connect(connection, &Audio::VoiceConnection::connected, this, &CallVideo::scheduleReplan);
    connection->client()->setVideoFrameHandler([this](Snowflake userId, Discord::Voice::VideoCodec codec, Discord::Voice::EncodedVideoFrame frame) {
        std::shared_ptr<Media::VideoDecoder> decoder;
        {
            std::lock_guard lock(decoderMutex);
            auto it = decoders.find(static_cast<quint64>(userId));
            if (it != decoders.end())
                decoder = it->second;
        }
        if (decoder)
            decoder->push(codec, std::move(frame));
    });
}

void CallVideo::detach()
{
    if (connection)
        connection->disconnect(this);
    connection = nullptr;
    othersOnServer.clear();
    replanTimer.stop();
    expiryTimer.stop();

    std::unordered_map<quint64, std::shared_ptr<Media::VideoDecoder>> stopping;
    {
        std::lock_guard lock(decoderMutex);
        stopping.swap(decoders);
    }
    stopping.clear();

    if (!layersByUser.isEmpty()) {
        layersByUser.clear();
        emit sendersChanged();
    }
}

QList<Snowflake> CallVideo::senders() const
{
    QList<Snowflake> users = layersByUser.keys();
    std::sort(users.begin(), users.end(), [](Snowflake a, Snowflake b) { return static_cast<quint64>(a) < static_cast<quint64>(b); });
    return users;
}

void CallVideo::onLayersChanged(Snowflake userId, const QList<Discord::Voice::VideoLayer> &layers)
{
    const bool wasSending = layersByUser.contains(userId);
    if (layers.isEmpty())
        layersByUser.remove(userId);
    else
        layersByUser.insert(userId, layers);

    scheduleReplan();
    if (wasSending != !layers.isEmpty())
        emit sendersChanged();
}

void CallVideo::setTile(Snowflake userId, std::shared_ptr<Media::VideoFrameSink> sink, int pixelCount, bool visible)
{
    Tile &tile = tiles[userId];
    tile.sink = std::move(sink);
    tile.pixels = pixelCount;
    visibility.setVisible(userId, visible, clock.elapsed());

    {
        std::lock_guard lock(decoderMutex);
        auto it = decoders.find(static_cast<quint64>(userId));
        if (it != decoders.end())
            it->second->setSink(tile.sink);
    }
    scheduleReplan();
}

void CallVideo::removeTile(Snowflake userId)
{
    if (!tiles.remove(userId))
        return;
    visibility.remove(userId);
    scheduleReplan();
}

void CallVideo::setFocused(Snowflake userId)
{
    if (focusedUser == userId)
        return;
    focusedUser = userId;
    scheduleReplan();
}

void CallVideo::onUserReachedServer(Snowflake userId)
{
    if (userId == ownUser || othersOnServer.contains(userId))
        return;
    othersOnServer.insert(userId);
    scheduleReplan();
}

void CallVideo::onUserLeftServer(Snowflake userId)
{
    if (othersOnServer.remove(userId))
        scheduleReplan();
}

bool CallVideo::isVideoDisabled(Snowflake userId) const
{
    return disabledUsers.contains(userId);
}

void CallVideo::setVideoDisabled(Snowflake userId, bool disabled)
{
    if (disabled == disabledUsers.contains(userId))
        return;
    if (disabled)
        disabledUsers.insert(userId);
    else
        disabledUsers.remove(userId);

    QStringList stored;
    for (Snowflake id : std::as_const(disabledUsers))
        stored.append(QString::number(static_cast<quint64>(id)));
    QSettings().setValue(DISABLED_USERS_KEY, stored);

    scheduleReplan();
    emit videoDisabledChanged(userId);
}

Media::VideoDecoder::Stats CallVideo::decoderStats(Snowflake userId) const
{
    std::lock_guard lock(decoderMutex);
    auto it = decoders.find(static_cast<quint64>(userId));
    return it != decoders.end() ? it->second->stats() : Media::VideoDecoder::Stats();
}

void CallVideo::scheduleReplan()
{
    if (connection && !replanTimer.isActive())
        replanTimer.start();
}

void CallVideo::replan()
{
    if (!connection)
        return;

    const qint64 now = clock.elapsed();
    const int decoderLimit = maxDecoders();

    Discord::Voice::SinkWantsInput input;
    input.focusedUser = focusedUser;
    input.oneToOne = othersOnServer.size() == 1;

    // The focused sender is considered first so that the decoder limit never leaves it out.
    QList<Snowflake> users = senders();
    const int focusedAt = users.indexOf(focusedUser);
    if (focusedAt > 0)
        users.move(focusedAt, 0);

    int wantedCount = 0;
    for (Snowflake userId : std::as_const(users)) {
        Discord::Voice::VideoSender sender;
        sender.userId = userId;
        sender.layers = layersByUser.value(userId);
        sender.tilePixels = tiles.value(userId).pixels;
        sender.wanted = tiles.contains(userId) && !disabledUsers.contains(userId) && visibility.wanted(userId, now) && wantedCount < decoderLimit;
        if (sender.wanted)
            wantedCount++;
        input.senders.append(sender);
    }

    const Discord::Voice::SinkWantsPlan plan = Discord::Voice::planSinkWants(input);

    // A decoder for every sender being received, and for nobody else.
    std::unordered_map<quint64, std::shared_ptr<Media::VideoDecoder>> retired;
    {
        std::lock_guard lock(decoderMutex);
        for (auto it = decoders.begin(); it != decoders.end();) {
            if (plan.deliver.contains(Snowflake(it->first))) {
                ++it;
                continue;
            }
            retired.insert(*it);
            it = decoders.erase(it);
        }
        for (auto it = plan.deliver.constBegin(); it != plan.deliver.constEnd(); ++it) {
            const quint64 key = static_cast<quint64>(it.key());
            if (decoders.count(key))
                continue;
            const int asked = plan.wants.bySsrc.value(it.value(), plan.wants.any.value_or(Discord::Voice::sinkWantLevel(1)));
            const bool onCard = Media::hardwareDecodingEnabled() && asked > MOST_ASKED_OF_A_PICTURE_DECODED_IN_MEMORY;
            // A camera is at most 720p, which one thread decodes with room to spare.
            auto decoder = std::make_shared<Media::VideoDecoder>(onCard, 1);
            decoder->setSink(tiles.value(it.key()).sink);
            // The decoders go before the client does: detach() is called once the connection
            // has stopped, which is before it is let go of.
            VoiceClient *client = connection->client();
            decoder->setKeyframeRequester([client, userId = it.key()] { QMetaObject::invokeMethod(client, [client, userId] { client->requestVideoKeyframeFrom(userId); }); });
            decoder->setCodecFailureHandler([client](Discord::Voice::VideoCodec codec) { QMetaObject::invokeMethod(client, [client, codec] { client->withdrawVideoCodec(codec); }); });
            decoders.emplace(key, std::move(decoder));
        }
    }
    retired.clear();

    QSet<quint32> deliver;
    for (quint32 ssrc : plan.deliver)
        deliver.insert(ssrc);
    connection->run([wants = plan.wants, deliver](VoiceClient *client) { client->setVideoPlan(wants, deliver); });

    if (visibility.hasPendingExpiry(now))
        expiryTimer.start();
    else
        expiryTimer.stop();
}

} // namespace Core
} // namespace Acheron
