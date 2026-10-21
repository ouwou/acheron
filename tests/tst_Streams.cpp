// expected payload bytes are what the official client sends (docs/video-re/re-main-gateway.md)

#include "Core/Streams/StreamManager.hpp"
#include "Core/Streams/StreamPreviews.hpp"
#include "Discord/Events.hpp"
#include "Discord/Outbound.hpp"
#include "Discord/StreamKey.hpp"

#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

using namespace Acheron;
using Core::Snowflake;
using Core::StreamEndReason;
using Core::StreamManager;
using Core::StreamPreviews;
using Core::StreamPreviewUploader;
using Core::StreamRecord;
using Discord::StreamKey;

namespace {

const QString GUILD_KEY = QStringLiteral("guild:111:222:333");
const QString CALL_KEY = QStringLiteral("call:222:333");

template <typename Event>
Event parseEvent(const char *json)
{
    return Event::fromJson(QJsonDocument::fromJson(json).object());
}

Discord::StreamCreate created(const QString &key = GUILD_KEY)
{
    return parseEvent<Discord::StreamCreate>(QStringLiteral(R"({"stream_key":"%1","region":"rotterdam","viewer_ids":["444"],"rtc_server_id":"900","rtc_channel_id":"899","paused":false})").arg(key).toUtf8().constData());
}

Discord::StreamServerUpdate serverUpdate(const char *endpoint)
{
    return parseEvent<Discord::StreamServerUpdate>(QStringLiteral(R"({"stream_key":"guild:111:222:333","endpoint":%1,"token":"stream-token"})").arg(QString::fromLatin1(endpoint)).toUtf8().constData());
}

struct FakeLookups
{
    QStringList requested;
    QList<std::function<void(const Discord::StreamPreviewLookup &)>> pending;

    StreamPreviews::Fetcher fetcher()
    {
        return [this](const QString &key, std::function<void(const Discord::StreamPreviewLookup &)> done) {
            requested.append(key);
            pending.append(std::move(done));
        };
    }

    void answer(const QString &url, int status = 200, qint64 retryAfterMs = 0)
    {
        Discord::StreamPreviewLookup lookup;
        lookup.url = QUrl(url);
        lookup.statusCode = status;
        lookup.retryAfterMs = retryAfterMs;
        pending.takeFirst()(lookup);
    }
};

struct FakeUploads
{
    QStringList keys;
    QStringList thumbnails;
    QList<std::function<void(bool)>> pending;

    StreamPreviewUploader::Upload uploader()
    {
        return [this](const QString &key, const QString &thumbnail, std::function<void(bool)> done) {
            keys.append(key);
            thumbnails.append(thumbnail);
            pending.append(std::move(done));
        };
    }

    void answer(bool uploaded) { pending.takeFirst()(uploaded); }
};

QImage screen(int width, int height)
{
    QImage picture(width, height, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    return picture;
}

QImage decodedThumbnail(const QString &thumbnail)
{
    const QString prefix = QStringLiteral("data:image/jpeg;base64,");
    if (!thumbnail.startsWith(prefix))
        return {};
    return QImage::fromData(QByteArray::fromBase64(thumbnail.mid(prefix.size()).toLatin1()), "JPEG");
}

} // namespace

class TestStreams : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();

    void keyFormatsGuildAndCallForms();
    void keyRoundTrips_data();
    void keyRoundTrips();
    void keyParseRejectsMalformed_data();
    void keyParseRejectsMalformed();

    void createPayloadForGuildChannel();
    void createPayloadForCall();
    void keyedPayloads();

    void recordFollowsCreateThenServerUpdate();
    void serverUpdateBeforeCreateIsIgnored();
    void nullEndpointMeansWait();
    void updateChangesViewersAndPause();
    void deleteEndsTheStreamWithItsReason_data();
    void deleteEndsTheStreamWithItsReason();
    void unavailableKeepsTheRecord();
    void eventsForUnknownStreamsAreIgnored();
    void unreadableKeyIsIgnored();
    void resetEndsEveryStream();

    void previewIsLookedUpOnce();
    void previewIsCachedForTwoMinutes();
    void failedLookupBacksOffLongerEachTime();
    void rateLimitWaitsAsLongAsAsked();
    void successClearsTheBackoff();
    void forgettingAnOwnerDropsOnlyTheirStreams();
    void lookupFinishingAfterForgetIsIgnored();

    void thumbnailIsAJpegFittedInside512x288_data();
    void thumbnailIsAJpegFittedInside512x288();
    void noThumbnailOfNoPicture();
    void firstThumbnailIsUploadedHalfASecondAfterTheVideoStarts();
    void nextThumbnailIsFiveMinutesAfterAnUpload();
    void failedUploadIsTriedAgainInAMinute();
    void stalledCaptureIsLookedAtAgainInAMinute();
    void uploadFinishingAfterTheStreamStoppedIsIgnored();
    void startingAgainStartsTheWaitAgain();
};

void TestStreams::initTestCase()
{
    qRegisterMetaType<StreamEndReason>();
}

void TestStreams::keyFormatsGuildAndCallForms()
{
    const StreamKey guild = StreamKey::forChannel(Snowflake(111), Snowflake(222), Snowflake(333));
    QCOMPARE(guild.type, StreamKey::Type::Guild);
    QCOMPARE(guild.toString(), GUILD_KEY);
    QCOMPARE(guild.typeName(), QStringLiteral("guild"));

    const StreamKey call = StreamKey::forChannel(Snowflake::Invalid, Snowflake(222), Snowflake(333));
    QCOMPARE(call.type, StreamKey::Type::Call);
    QCOMPARE(call.toString(), CALL_KEY);
    QCOMPARE(call.typeName(), QStringLiteral("call"));
}

void TestStreams::keyRoundTrips_data()
{
    QTest::addColumn<QString>("key");

    QTest::newRow("guild") << GUILD_KEY;
    QTest::newRow("call") << CALL_KEY;
    QTest::newRow("guild, real-sized ids") << QStringLiteral("guild:1098765432109876543:1198765432109876544:398765432109876545");
    QTest::newRow("call, largest id") << QStringLiteral("call:18446744073709551614:1");
}

void TestStreams::keyRoundTrips()
{
    QFETCH(QString, key);

    const auto parsed = StreamKey::parse(key);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->toString(), key);
}

void TestStreams::keyParseRejectsMalformed_data()
{
    QTest::addColumn<QString>("key");

    QTest::newRow("empty") << QString();
    QTest::newRow("unknown type") << QStringLiteral("stage:111:222:333");
    QTest::newRow("guild missing owner") << QStringLiteral("guild:111:222");
    QTest::newRow("guild with extra part") << QStringLiteral("guild:111:222:333:444");
    QTest::newRow("call with guild part") << QStringLiteral("call:111:222:333");
    QTest::newRow("call missing owner") << QStringLiteral("call:222");
    QTest::newRow("non-numeric id") << QStringLiteral("guild:111:abc:333");
    QTest::newRow("empty id") << QStringLiteral("guild:111::333");
    QTest::newRow("zero id") << QStringLiteral("call:0:333");
    QTest::newRow("negative id") << QStringLiteral("call:-5:333");
    QTest::newRow("type only") << QStringLiteral("guild");
    QTest::newRow("wrong case") << QStringLiteral("Guild:111:222:333");
}

void TestStreams::keyParseRejectsMalformed()
{
    QFETCH(QString, key);
    QVERIFY(!StreamKey::parse(key).has_value());
}

void TestStreams::createPayloadForGuildChannel()
{
    Discord::CreateStream msg;
    msg.type = QStringLiteral("guild");
    msg.guildId = Snowflake(111);
    msg.channelId = Snowflake(222);
    msg.preferredRegion = nullptr;
    QCOMPARE(msg.toJson().toBytes(), QByteArray(R"({"op":18,"d":{"type":"guild","guild_id":"111","channel_id":"222","preferred_region":null}})"));

    msg.preferredRegion = QStringLiteral("rotterdam");
    QCOMPARE(msg.toJson().toBytes(), QByteArray(R"({"op":18,"d":{"type":"guild","guild_id":"111","channel_id":"222","preferred_region":"rotterdam"}})"));
}

void TestStreams::createPayloadForCall()
{
    Discord::CreateStream msg;
    msg.type = QStringLiteral("call");
    msg.guildId = nullptr;
    msg.channelId = Snowflake(222);
    msg.preferredRegion = nullptr;
    QCOMPARE(msg.toJson().toBytes(), QByteArray(R"({"op":18,"d":{"type":"call","guild_id":null,"channel_id":"222","preferred_region":null}})"));
}

void TestStreams::keyedPayloads()
{
    Discord::DeleteStream stop;
    stop.streamKey = GUILD_KEY;
    QCOMPARE(stop.toJson().toBytes(), QByteArray(R"({"op":19,"d":{"stream_key":"guild:111:222:333"}})"));

    Discord::WatchStream watch;
    watch.streamKey = GUILD_KEY;
    QCOMPARE(watch.toJson().toBytes(), QByteArray(R"({"op":20,"d":{"stream_key":"guild:111:222:333"}})"));

    Discord::PingStream ping;
    ping.streamKey = CALL_KEY;
    QCOMPARE(ping.toJson().toBytes(), QByteArray(R"({"op":21,"d":{"stream_key":"call:222:333"}})"));

    Discord::SetStreamPaused pause;
    pause.streamKey = GUILD_KEY;
    pause.paused = true;
    QCOMPARE(pause.toJson().toBytes(), QByteArray(R"({"op":22,"d":{"stream_key":"guild:111:222:333","paused":true}})"));
}

void TestStreams::recordFollowsCreateThenServerUpdate()
{
    StreamManager manager;
    QSignalSpy changed(&manager, &StreamManager::streamChanged);

    manager.handleStreamCreate(created());

    auto record = manager.stream(GUILD_KEY);
    QVERIFY(record.has_value());
    QCOMPARE(record->state, StreamRecord::State::AwaitingEndpoint);
    QCOMPARE(record->key.toString(), GUILD_KEY);
    QCOMPARE(record->region, QStringLiteral("rotterdam"));
    QCOMPARE(record->viewerIds, QList<Snowflake>{ Snowflake(444) });
    QCOMPARE(quint64(record->rtcServerId), quint64(900));
    QCOMPARE(quint64(record->rtcChannelId), quint64(899));
    QVERIFY(record->endpoint.isEmpty());
    QCOMPARE(changed.size(), 1);

    manager.handleStreamServerUpdate(serverUpdate(R"("rotterdam123.discord.media:443")"));

    record = manager.stream(GUILD_KEY);
    QCOMPARE(record->state, StreamRecord::State::Ready);
    QCOMPARE(record->endpoint, QStringLiteral("rotterdam123.discord.media:443"));
    QCOMPARE(record->token, QStringLiteral("stream-token"));
    QCOMPARE(changed.size(), 2);
    QCOMPARE(changed.last().at(0).toString(), GUILD_KEY);
}

void TestStreams::serverUpdateBeforeCreateIsIgnored()
{
    // the official client drops a server update for a stream it has no connection object for, so an
    // endpoint never makes a record by itself
    StreamManager manager;
    QSignalSpy changed(&manager, &StreamManager::streamChanged);

    manager.handleStreamServerUpdate(serverUpdate(R"("rotterdam123.discord.media:443")"));

    QVERIFY(!manager.stream(GUILD_KEY).has_value());
    QVERIFY(manager.streamKeys().isEmpty());
    QVERIFY(changed.isEmpty());
}

void TestStreams::nullEndpointMeansWait()
{
    StreamManager manager;
    manager.handleStreamCreate(created());
    manager.handleStreamServerUpdate(serverUpdate(R"("rotterdam123.discord.media:443")"));

    manager.handleStreamServerUpdate(serverUpdate("null"));

    const auto record = manager.stream(GUILD_KEY);
    QVERIFY(record.has_value());
    QCOMPARE(record->state, StreamRecord::State::AwaitingEndpoint);
    QVERIFY(record->endpoint.isEmpty());
}

void TestStreams::updateChangesViewersAndPause()
{
    StreamManager manager;
    manager.handleStreamCreate(created());
    manager.handleStreamServerUpdate(serverUpdate(R"("rotterdam123.discord.media:443")"));

    manager.handleStreamUpdate(parseEvent<Discord::StreamUpdate>(R"({"stream_key":"guild:111:222:333","region":"frankfurt","viewer_ids":["444","555"],"paused":true})"));

    const auto record = manager.stream(GUILD_KEY);
    QCOMPARE(record->viewerIds, (QList<Snowflake>{ Snowflake(444), Snowflake(555) }));
    QCOMPARE(record->paused, true);
    QCOMPARE(record->region, QStringLiteral("frankfurt"));
    QCOMPARE(record->state, StreamRecord::State::Ready);
    QCOMPARE(record->endpoint, QStringLiteral("rotterdam123.discord.media:443"));
}

void TestStreams::deleteEndsTheStreamWithItsReason_data()
{
    QTest::addColumn<QByteArray>("event");
    QTest::addColumn<StreamEndReason>("reason");

    QTest::newRow("user requested") << QByteArray(R"({"stream_key":"guild:111:222:333","reason":"user_requested"})") << StreamEndReason::UserRequested;
    QTest::newRow("stream full") << QByteArray(R"({"stream_key":"guild:111:222:333","reason":"stream_full"})") << StreamEndReason::StreamFull;
    QTest::newRow("unauthorized") << QByteArray(R"({"stream_key":"guild:111:222:333","reason":"unauthorized"})") << StreamEndReason::Unauthorized;
    QTest::newRow("rate limited") << QByteArray(R"({"stream_key":"guild:111:222:333","reason":"safety_guild_rate_limited"})") << StreamEndReason::SafetyGuildRateLimited;
    QTest::newRow("reason the client has no name for") << QByteArray(R"({"stream_key":"guild:111:222:333","reason":"stream_ended"})") << StreamEndReason::Other;
    QTest::newRow("no reason") << QByteArray(R"({"stream_key":"guild:111:222:333"})") << StreamEndReason::Other;
    QTest::newRow("unavailable false") << QByteArray(R"({"stream_key":"guild:111:222:333","unavailable":false,"reason":"user_requested"})") << StreamEndReason::UserRequested;
}

void TestStreams::deleteEndsTheStreamWithItsReason()
{
    QFETCH(QByteArray, event);
    QFETCH(StreamEndReason, reason);

    StreamManager manager;
    manager.handleStreamCreate(created());
    QSignalSpy ended(&manager, &StreamManager::streamEnded);

    manager.handleStreamDelete(parseEvent<Discord::StreamDelete>(event.constData()));

    QVERIFY(!manager.stream(GUILD_KEY).has_value());
    QCOMPARE(ended.size(), 1);
    QCOMPARE(ended.first().at(0).toString(), GUILD_KEY);
    QCOMPARE(ended.first().at(1).value<StreamEndReason>(), reason);
}

void TestStreams::unavailableKeepsTheRecord()
{
    StreamManager manager;
    manager.handleStreamCreate(created());
    manager.handleStreamServerUpdate(serverUpdate(R"("rotterdam123.discord.media:443")"));
    QSignalSpy ended(&manager, &StreamManager::streamEnded);

    manager.handleStreamDelete(parseEvent<Discord::StreamDelete>(R"({"stream_key":"guild:111:222:333","unavailable":true})"));

    auto record = manager.stream(GUILD_KEY);
    QVERIFY(record.has_value());
    QCOMPARE(record->state, StreamRecord::State::Reconnecting);
    QVERIFY(record->endpoint.isEmpty());
    QVERIFY(ended.isEmpty());

    manager.handleStreamCreate(created());
    QCOMPARE(manager.stream(GUILD_KEY)->state, StreamRecord::State::AwaitingEndpoint);
}

void TestStreams::eventsForUnknownStreamsAreIgnored()
{
    StreamManager manager;
    QSignalSpy changed(&manager, &StreamManager::streamChanged);
    QSignalSpy ended(&manager, &StreamManager::streamEnded);

    manager.handleStreamUpdate(parseEvent<Discord::StreamUpdate>(R"({"stream_key":"guild:111:222:333","region":"x","viewer_ids":[],"paused":true})"));
    manager.handleStreamDelete(parseEvent<Discord::StreamDelete>(R"({"stream_key":"guild:111:222:333","reason":"user_requested"})"));

    QVERIFY(changed.isEmpty());
    QVERIFY(ended.isEmpty());
    QVERIFY(manager.streamKeys().isEmpty());
}

void TestStreams::unreadableKeyIsIgnored()
{
    StreamManager manager;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression("STREAM_CREATE with a stream key this client cannot read"));
    manager.handleStreamCreate(created(QStringLiteral("stage:111:222:333")));
    QVERIFY(manager.streamKeys().isEmpty());
}

void TestStreams::resetEndsEveryStream()
{
    StreamManager manager;
    manager.handleStreamCreate(created(GUILD_KEY));
    manager.handleStreamCreate(created(CALL_KEY));
    QSignalSpy ended(&manager, &StreamManager::streamEnded);

    manager.reset();

    QVERIFY(manager.streamKeys().isEmpty());
    QCOMPARE(ended.size(), 2);
    QStringList endedKeys{ ended.at(0).at(0).toString(), ended.at(1).at(0).toString() };
    endedKeys.sort();
    QCOMPARE(endedKeys, (QStringList{ CALL_KEY, GUILD_KEY }));
}

void TestStreams::previewIsLookedUpOnce()
{
    FakeLookups lookups;
    qint64 now = 1'000'000;
    StreamPreviews previews(lookups.fetcher(), nullptr, [&] { return now; });
    QSignalSpy changed(&previews, &StreamPreviews::previewChanged);

    QVERIFY(previews.request(GUILD_KEY).isEmpty());
    QVERIFY(previews.request(GUILD_KEY).isEmpty());
    QCOMPARE(lookups.requested, QStringList{ GUILD_KEY });

    lookups.answer("https://media.example/preview.jpg");

    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.first().at(0).toString(), GUILD_KEY);
    QCOMPARE(changed.first().at(1).toUrl(), QUrl("https://media.example/preview.jpg"));
    QCOMPARE(previews.request(GUILD_KEY), QUrl("https://media.example/preview.jpg"));
    QCOMPARE(lookups.requested.size(), 1);
}

void TestStreams::previewIsCachedForTwoMinutes()
{
    FakeLookups lookups;
    qint64 now = 1'000'000;
    StreamPreviews previews(lookups.fetcher(), nullptr, [&] { return now; });

    previews.request(GUILD_KEY);
    lookups.answer("https://media.example/first.jpg");

    now += StreamPreviews::FRESH_FOR_MS - 1;
    QCOMPARE(previews.request(GUILD_KEY), QUrl("https://media.example/first.jpg"));
    QCOMPARE(lookups.requested.size(), 1);

    now += 1;
    QVERIFY(previews.request(GUILD_KEY).isEmpty());
    QCOMPARE(lookups.requested.size(), 2);

    lookups.answer("https://media.example/second.jpg");
    QCOMPARE(previews.request(GUILD_KEY), QUrl("https://media.example/second.jpg"));
}

void TestStreams::failedLookupBacksOffLongerEachTime()
{
    FakeLookups lookups;
    qint64 now = 1'000'000;
    StreamPreviews previews(lookups.fetcher(), nullptr, [&] { return now; });
    QSignalSpy changed(&previews, &StreamPreviews::previewChanged);

    for (int attempt = 1; attempt <= 3; ++attempt) {
        QVERIFY(previews.request(GUILD_KEY).isEmpty());
        QCOMPARE(lookups.requested.size(), attempt);
        lookups.answer(QString(), 404);
        QCOMPARE(changed.last().at(1).toUrl(), QUrl());

        now += StreamPreviews::BACKOFF_STEP_MS * attempt - 1;
        previews.request(GUILD_KEY);
        QCOMPARE(lookups.requested.size(), attempt);

        now += 1;
    }

    previews.request(GUILD_KEY);
    QCOMPARE(lookups.requested.size(), 4);
}

void TestStreams::rateLimitWaitsAsLongAsAsked()
{
    FakeLookups lookups;
    qint64 now = 1'000'000;
    StreamPreviews previews(lookups.fetcher(), nullptr, [&] { return now; });

    previews.request(GUILD_KEY);
    lookups.answer(QString(), 429, 45'000);

    now += 44'999;
    previews.request(GUILD_KEY);
    QCOMPARE(lookups.requested.size(), 1);

    now += 1;
    previews.request(GUILD_KEY);
    QCOMPARE(lookups.requested.size(), 2);
}

void TestStreams::successClearsTheBackoff()
{
    FakeLookups lookups;
    qint64 now = 1'000'000;
    StreamPreviews previews(lookups.fetcher(), nullptr, [&] { return now; });

    previews.request(GUILD_KEY);
    lookups.answer(QString(), 500);
    now += StreamPreviews::BACKOFF_STEP_MS;
    previews.request(GUILD_KEY);
    lookups.answer("https://media.example/preview.jpg");

    now += StreamPreviews::FRESH_FOR_MS;
    previews.request(GUILD_KEY);
    lookups.answer(QString(), 500);
    now += StreamPreviews::BACKOFF_STEP_MS;
    previews.request(GUILD_KEY);
    QCOMPARE(lookups.requested.size(), 4);
}

void TestStreams::forgettingAnOwnerDropsOnlyTheirStreams()
{
    FakeLookups lookups;
    qint64 now = 1'000'000;
    StreamPreviews previews(lookups.fetcher(), nullptr, [&] { return now; });

    const QString otherOwner = QStringLiteral("guild:111:222:777");
    previews.request(GUILD_KEY);
    lookups.answer("https://media.example/owner333.jpg");
    previews.request(otherOwner);
    lookups.answer("https://media.example/owner777.jpg");

    previews.forgetOwner(Snowflake(333));

    QCOMPARE(previews.request(otherOwner), QUrl("https://media.example/owner777.jpg"));
    QVERIFY(previews.request(GUILD_KEY).isEmpty());
    QCOMPARE(lookups.requested, (QStringList{ GUILD_KEY, otherOwner, GUILD_KEY }));
}

void TestStreams::lookupFinishingAfterForgetIsIgnored()
{
    FakeLookups lookups;
    qint64 now = 1'000'000;
    StreamPreviews previews(lookups.fetcher(), nullptr, [&] { return now; });
    QSignalSpy changed(&previews, &StreamPreviews::previewChanged);

    previews.request(GUILD_KEY);
    previews.forgetOwner(Snowflake(333));
    lookups.answer("https://media.example/stale.jpg");

    QVERIFY(changed.isEmpty());
    QVERIFY(previews.request(GUILD_KEY).isEmpty());
    QCOMPARE(lookups.requested.size(), 2);
}

void TestStreams::thumbnailIsAJpegFittedInside512x288_data()
{
    QTest::addColumn<QSize>("source");
    QTest::addColumn<QSize>("thumbnail");

    QTest::newRow("16:9") << QSize(1920, 1080) << QSize(512, 288);
    QTest::newRow("4:3") << QSize(1024, 768) << QSize(384, 288);
    QTest::newRow("ultrawide") << QSize(2560, 1080) << QSize(512, 216);
    QTest::newRow("portrait") << QSize(1080, 1920) << QSize(162, 288);
    QTest::newRow("smaller than the bound") << QSize(256, 144) << QSize(512, 288);
}

void TestStreams::thumbnailIsAJpegFittedInside512x288()
{
    QFETCH(QSize, source);
    QFETCH(QSize, thumbnail);

    const QImage decoded = decodedThumbnail(StreamPreviewUploader::thumbnailOf(screen(source.width(), source.height())));
    QCOMPARE(decoded.size(), thumbnail);
}

void TestStreams::noThumbnailOfNoPicture()
{
    QVERIFY(StreamPreviewUploader::thumbnailOf(QImage()).isEmpty());
}

void TestStreams::firstThumbnailIsUploadedHalfASecondAfterTheVideoStarts()
{
    FakeUploads uploads;
    StreamPreviewUploader uploader(uploads.uploader());
    QCOMPARE(uploader.waitMs(), std::optional<int>());

    uploader.start(GUILD_KEY);
    QCOMPARE(uploader.waitMs(), std::optional<int>(500));
    uploader.offer(screen(640, 360));
    QVERIFY(uploads.keys.isEmpty());

    QTRY_COMPARE(uploader.waitMs(), std::optional<int>());
    uploader.offer(screen(640, 360));
    QCOMPARE(uploads.keys, QStringList{ GUILD_KEY });
    QCOMPARE(decodedThumbnail(uploads.thumbnails.first()).size(), QSize(512, 288));

    uploader.offer(screen(640, 360));
    QCOMPARE(uploads.keys.size(), 1);
}

void TestStreams::nextThumbnailIsFiveMinutesAfterAnUpload()
{
    FakeUploads uploads;
    StreamPreviewUploader uploader(uploads.uploader());
    uploader.start(GUILD_KEY);
    QTRY_COMPARE(uploader.waitMs(), std::optional<int>());
    uploader.offer(screen(640, 360));

    uploads.answer(true);
    QCOMPARE(uploader.waitMs(), std::optional<int>(5 * 60 * 1000));
    uploader.offer(screen(640, 360));
    QCOMPARE(uploads.keys.size(), 1);
}

void TestStreams::failedUploadIsTriedAgainInAMinute()
{
    FakeUploads uploads;
    StreamPreviewUploader uploader(uploads.uploader());
    uploader.start(GUILD_KEY);
    QTRY_COMPARE(uploader.waitMs(), std::optional<int>());
    uploader.offer(screen(640, 360));

    uploads.answer(false);
    QCOMPARE(uploader.waitMs(), std::optional<int>(60 * 1000));
}

void TestStreams::stalledCaptureIsLookedAtAgainInAMinute()
{
    FakeUploads uploads;
    StreamPreviewUploader uploader(uploads.uploader());
    uploader.setStalled(true);
    uploader.start(GUILD_KEY);

    QTRY_COMPARE(uploader.waitMs(), std::optional<int>(60 * 1000));
    uploader.offer(screen(640, 360));
    QVERIFY(uploads.keys.isEmpty());
}

void TestStreams::uploadFinishingAfterTheStreamStoppedIsIgnored()
{
    FakeUploads uploads;
    StreamPreviewUploader uploader(uploads.uploader());
    uploader.start(GUILD_KEY);
    QTRY_COMPARE(uploader.waitMs(), std::optional<int>());
    uploader.offer(screen(640, 360));

    uploader.stop();
    uploads.answer(true);
    QCOMPARE(uploader.waitMs(), std::optional<int>());
    uploader.offer(screen(640, 360));
    QCOMPARE(uploads.keys.size(), 1);
}

void TestStreams::startingAgainStartsTheWaitAgain()
{
    FakeUploads uploads;
    StreamPreviewUploader uploader(uploads.uploader());
    uploader.start(GUILD_KEY);
    QTRY_COMPARE(uploader.waitMs(), std::optional<int>());
    uploader.offer(screen(640, 360));
    uploads.answer(true);
    QCOMPARE(uploader.waitMs(), std::optional<int>(5 * 60 * 1000));

    uploader.start(CALL_KEY);
    QCOMPARE(uploader.waitMs(), std::optional<int>(500));
    QTRY_COMPARE(uploader.waitMs(), std::optional<int>());
    uploader.offer(screen(640, 360));
    QCOMPARE(uploads.keys, (QStringList{ GUILD_KEY, CALL_KEY }));
}

QTEST_GUILESS_MAIN(TestStreams)
#include "tst_Streams.moc"
