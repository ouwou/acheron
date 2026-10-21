#include "CallVideo.hpp"

#include "Core/Audio/VoiceConnection.hpp"

#include <QSettings>

#include <algorithm>
#include <optional>

namespace Acheron {
namespace Core {

using Discord::Voice::VoiceClient;

namespace {

const char *const DISABLED_USERS_KEY = "video/disabled_users";
const char *const MAX_DECODERS_KEY = "video/max_decoders";

// a card hand-over costs the same at any size, which for small pictures is more than decoding in
// memory: measured with nine tiles on a Radeon RX 7800 XT, 1.0 ms of CPU a picture against 0.6 ms at
// 640x360
constexpr int MOST_ASKED_OF_A_PICTURE_DECODED_IN_MEMORY = 70;
// a call can show many cameras at once, so each decodes on one thread
constexpr int SOFTWARE_THREADS_PER_CAMERA = 1;
constexpr int EXPIRY_CHECK_MS = 1000;

std::optional<int> knownMaxDecoders;
QList<CallVideo *> &allCallVideos()
{
    static QList<CallVideo *> instances;
    return instances;
}

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

    expiryTimer.setInterval(EXPIRY_CHECK_MS);
    connect(&expiryTimer, &QTimer::timeout, this, &CallVideo::replan);
    allCallVideos().append(this);
}

CallVideo::~CallVideo()
{
    allCallVideos().removeAll(this);
    detach();
}

int CallVideo::maxDecoders()
{
    if (!knownMaxDecoders)
        knownMaxDecoders = std::max(1, QSettings().value(MAX_DECODERS_KEY, DEFAULT_MAX_DECODERS).toInt());
    return *knownMaxDecoders;
}

void CallVideo::setMaxDecoders(int decoders)
{
    knownMaxDecoders = std::max(1, decoders);
    QSettings().setValue(MAX_DECODERS_KEY, *knownMaxDecoders);
    for (CallVideo *video : std::as_const(allCallVideos()))
        video->scheduleReplan();
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
        if (const std::shared_ptr<Media::VideoDecoder> decoder = decoderOf(userId))
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

    QHash<Snowflake, std::shared_ptr<Media::VideoDecoder>> stopping;
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
    std::sort(users.begin(), users.end());
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

    if (const std::shared_ptr<Media::VideoDecoder> decoder = decoderOf(userId))
        decoder->setSink(tile.sink);
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
    const std::shared_ptr<Media::VideoDecoder> decoder = decoderOf(userId);
    return decoder ? decoder->stats() : Media::VideoDecoder::Stats();
}

std::shared_ptr<Media::VideoDecoder> CallVideo::decoderOf(Snowflake userId) const
{
    std::lock_guard lock(decoderMutex);
    return decoders.value(userId);
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

    QHash<Snowflake, std::shared_ptr<Media::VideoDecoder>> retired;
    {
        std::lock_guard lock(decoderMutex);
        for (auto it = decoders.begin(); it != decoders.end();) {
            if (plan.deliver.contains(it.key())) {
                ++it;
                continue;
            }
            retired.insert(it.key(), it.value());
            it = decoders.erase(it);
        }
        for (auto it = plan.deliver.constBegin(); it != plan.deliver.constEnd(); ++it) {
            if (decoders.contains(it.key()))
                continue;
            const int asked = plan.wants.bySsrc.value(it.value(), plan.wants.any.value_or(Discord::Voice::sinkWantLevel(1)));
            const bool onCard = Media::hardwareDecodingEnabled() && asked > MOST_ASKED_OF_A_PICTURE_DECODED_IN_MEMORY;
            auto decoder = std::make_shared<Media::VideoDecoder>(onCard, SOFTWARE_THREADS_PER_CAMERA);
            decoder->setSink(tiles.value(it.key()).sink);
            // the decoders go before the client: detach() runs once the connection has stopped
            VoiceClient *client = connection->client();
            decoder->setKeyframeRequester([client, userId = it.key()] { QMetaObject::invokeMethod(client, [client, userId] { client->requestVideoKeyframeFrom(userId); }); });
            decoder->setCodecFailureHandler(client->queuedCodecWithdrawal());
            decoders.insert(it.key(), std::move(decoder));
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
