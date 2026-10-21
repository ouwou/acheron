// expected bytes are what the official desktop client's JSON.stringify produces (key order included),
// taken from the canary client's RTC control socket; see docs/video-re/re-voice-gateway-video.md

#include "Discord/Enums.hpp"
#include "Discord/Outbound.hpp"
#include "Discord/Voice/VoiceEntities.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace Acheron;
using namespace Acheron::Discord::Voice;

namespace {

QJsonObject parse(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}

StreamDescription offeredStream(const QString &type, const QString &rid, int quality)
{
    StreamDescription stream;
    stream.type = type;
    stream.rid = rid;
    stream.quality = quality;
    return stream;
}

VoiceIdentifyData identifyFor(quint64 serverId, quint64 channelId)
{
    VoiceIdentifyData identify;
    identify.serverId = Core::Snowflake(serverId);
    identify.channelId = Core::Snowflake(channelId);
    identify.userId = Core::Snowflake(300);
    identify.sessionId = QStringLiteral("session");
    identify.token = QStringLiteral("token");
    return identify;
}

Codec videoCodec(const QString &name, int index, bool encode, bool decode)
{
    Codec codec;
    codec.name = name;
    codec.type = QStringLiteral("video");
    codec.priority = (index + 1) * 1000;
    codec.payloadType = 101 + 2 * index;
    codec.rtxPayloadType = 102 + 2 * index;
    codec.encode = encode;
    codec.decode = decode;
    return codec;
}

} // namespace

class TestVoicePayloads : public QObject
{
    Q_OBJECT
private slots:
    void identifyWithoutVideoOmitsVideoKeys();
    void identifyVoiceConnectionOffersOneCameraLayer();

    void selectProtocolAudioOnly();
    void selectProtocolWithVideoCodecs();

    void localVideoStateKeyOrder();
    void localVideoStateCameraOff();

    void sinkWantsPutSsrcsBeforeAny();
    void sinkWantsForStreamViewer();
    void sinkWantsParse();

    void videoFromServerParsesUserSsrcs();
    void sessionUpdateParsesOnlyWhatIsPresent();

    void voiceStateUpdateSaysWhetherTheCameraIsOn();
    void voiceStateUpdateForAPrivateCallAndForLeaving();
    void voiceStateUpdateKeyOrderWithFlags();
};

void TestVoicePayloads::identifyWithoutVideoOmitsVideoKeys()
{
    QCOMPARE(identifyFor(100, 200).toJson().toBytes(),
             QByteArray(R"({"server_id":"100","channel_id":"200","user_id":"300","session_id":"session","token":"token","max_dave_protocol_version":1})"));
}

// the official client with simulcast off. with it on, rid "50" at quality 50 follows
void TestVoicePayloads::identifyVoiceConnectionOffersOneCameraLayer()
{
    VoiceIdentifyData identify = identifyFor(100, 200);
    identify.video = true;
    identify.streams = QList<StreamDescription>{ offeredStream("video", "100", 100) };

    QCOMPARE(identify.toJson().toBytes(),
             QByteArray(R"({"server_id":"100","channel_id":"200","user_id":"300","session_id":"session","token":"token","max_dave_protocol_version":1,"video":true,"streams":[{"type":"video","rid":"100","quality":100}]})"));
}

void TestVoicePayloads::selectProtocolAudioOnly()
{
    SelectProtocolData protocol;
    protocol.address = QStringLiteral("203.0.113.7");
    protocol.port = 50123;
    protocol.mode = QStringLiteral("aead_aes256_gcm_rtpsize");
    protocol.codecs = QList<Codec>{ Codec::opus() };

    QCOMPARE(protocol.toJson().toBytes(),
             QByteArray(R"({"protocol":"udp","data":{"address":"203.0.113.7","port":50123,"mode":"aead_aes256_gcm_rtpsize"},"address":"203.0.113.7","port":50123,"mode":"aead_aes256_gcm_rtpsize","codecs":[{"name":"opus","type":"audio","priority":1000,"payload_type":120}]})"));
}

void TestVoicePayloads::selectProtocolWithVideoCodecs()
{
    SelectProtocolData protocol;
    protocol.address = QStringLiteral("203.0.113.7");
    protocol.port = 50123;
    protocol.mode = QStringLiteral("aead_xchacha20_poly1305_rtpsize");
    protocol.codecs = QList<Codec>{ Codec::opus(), videoCodec("H264", 0, false, true), videoCodec("VP8", 1, false, true) };
    protocol.rtcConnectionId = QStringLiteral("0b0f3c2e-8f5c-4d5e-9a57-3c1f6a2d9e10");
    protocol.experiments = QStringList{ QStringLiteral("fixed_keyframe_interval") };

    QCOMPARE(protocol.toJson().toBytes(),
             QByteArray(R"({"protocol":"udp","data":{"address":"203.0.113.7","port":50123,"mode":"aead_xchacha20_poly1305_rtpsize"},"address":"203.0.113.7","port":50123,"mode":"aead_xchacha20_poly1305_rtpsize","codecs":[{"name":"opus","type":"audio","priority":1000,"payload_type":120},{"name":"H264","type":"video","priority":1000,"payload_type":101,"rtx_payload_type":102,"encode":false,"decode":true},{"name":"VP8","type":"video","priority":2000,"payload_type":103,"rtx_payload_type":104,"encode":false,"decode":true}],"rtc_connection_id":"0b0f3c2e-8f5c-4d5e-9a57-3c1f6a2d9e10","experiments":["fixed_keyframe_interval"]})"));
}

void TestVoicePayloads::localVideoStateKeyOrder()
{
    StreamResolution resolution;
    resolution.type = QStringLiteral("fixed");
    resolution.width = 1280;
    resolution.height = 720;

    StreamDescription stream;
    stream.type = QStringLiteral("video");
    stream.rid = QStringLiteral("100");
    stream.ssrc = 4001u;
    stream.active = true;
    stream.quality = 100;
    stream.rtxSsrc = 4002u;
    stream.maxBitrate = 2500000;
    stream.maxFramerate = 30;
    stream.maxResolution = resolution;

    LocalVideoState state;
    state.audioSsrc = 4000u;
    state.videoSsrc = 4001u;
    state.rtxSsrc = 4002u;
    state.streams = QList<StreamDescription>{ stream };

    QCOMPARE(state.toJson().toBytes(),
             QByteArray(R"({"audio_ssrc":4000,"video_ssrc":4001,"rtx_ssrc":4002,"streams":[{"type":"video","rid":"100","ssrc":4001,"active":true,"quality":100,"rtx_ssrc":4002,"max_bitrate":2500000,"max_framerate":30,"max_resolution":{"type":"fixed","width":1280,"height":720}}]})"));
}

void TestVoicePayloads::localVideoStateCameraOff()
{
    LocalVideoState state;
    state.audioSsrc = 4000u;
    state.videoSsrc = 0u;
    state.rtxSsrc = 0u;

    QCOMPARE(state.toJson().toBytes(), QByteArray(R"({"audio_ssrc":4000,"video_ssrc":0,"rtx_ssrc":0,"streams":[]})"));
}

void TestVoicePayloads::sinkWantsPutSsrcsBeforeAny()
{
    MediaSinkWants wants;
    wants.any = 50;
    wants.bySsrc.insert(9000, 100);
    wants.bySsrc.insert(2000, 0);

    QCOMPARE(wants.toJson().toBytes(), QByteArray(R"({"2000":0,"9000":100,"any":50})"));
}

void TestVoicePayloads::sinkWantsForStreamViewer()
{
    MediaSinkWants wants;
    wants.bySsrc.insert(7001, 100);
    wants.pixelCounts.insert(7001, 1920 * 1080);
    QCOMPARE(wants.toJson().toBytes(), QByteArray(R"({"7001":100,"pixelCounts":{"7001":2073600}})"));

    MediaSinkWants hidden;
    hidden.bySsrc.insert(7001, 0);
    QCOMPARE(hidden.toJson().toBytes(), QByteArray(R"({"7001":0})"));
}

void TestVoicePayloads::sinkWantsParse()
{
    const MediaSinkWants wants = MediaSinkWants::fromJson(parse(R"({"any":70,"4001":100,"4003":0,"pixelCounts":{"4001":921600}})"));

    QCOMPARE(wants.any.value_or(-1), 70);
    QCOMPARE(wants.bySsrc.size(), 2);
    QCOMPARE(wants.bySsrc.value(4001), 100);
    QCOMPARE(wants.bySsrc.value(4003), 0);
    QCOMPARE(wants.pixelCounts.value(4001), 921600);

    const MediaSinkWants bare = MediaSinkWants::fromJson(parse(R"({"4001":30})"));
    QVERIFY(!bare.any.has_value());
    QCOMPARE(bare.bySsrc.value(4001), 30);
    QVERIFY(bare.pixelCounts.isEmpty());
}

void TestVoicePayloads::videoFromServerParsesUserSsrcs()
{
    const ClientConnectData video = ClientConnectData::fromJson(parse(R"({"user_id":"123456789012345678","audio_ssrc":5000,"video_ssrc":5001,"streams":[{"type":"video","rid":"100","ssrc":5001,"rtx_ssrc":5002,"active":true,"quality":100,"max_bitrate":2500000,"max_framerate":30,"max_resolution":{"type":"fixed","width":1280,"height":720}}]})"));

    QCOMPARE(quint64(video.userId.get()), quint64(123456789012345678));
    QCOMPARE(video.audioSsrc.get(), quint32(5000));
    QCOMPARE(video.videoSsrc.get(), quint32(5001));
    QVERIFY(video.streams.hasValue());
    QCOMPARE(video.streams->size(), 1);

    const StreamDescription &stream = video.streams->at(0);
    QCOMPARE(stream.active.get(), true);
    QCOMPARE(stream.maxBitrate.get(), 2500000);
    QCOMPARE(stream.maxFramerate.get(), 30);
    QVERIFY(stream.maxResolution.hasValue());
    QCOMPARE(stream.maxResolution->type.get(), QStringLiteral("fixed"));
    QCOMPARE(stream.maxResolution->width.get(), 1280);
    QCOMPARE(stream.maxResolution->height.get(), 720);

    const ClientConnectData off = ClientConnectData::fromJson(parse(R"({"user_id":"123456789012345678","audio_ssrc":5000,"video_ssrc":0})"));
    QCOMPARE(off.videoSsrc.get(), quint32(0));
    QVERIFY(!off.streams.hasValue());
}

void TestVoicePayloads::sessionUpdateParsesOnlyWhatIsPresent()
{
    const SessionUpdate codecChange = SessionUpdate::fromJson(parse(R"({"video_codec":"VP8"})"));
    QCOMPARE(codecChange.videoCodec.get(), QStringLiteral("VP8"));
    QVERIFY(!codecChange.audioCodec.hasValue());
    QVERIFY(!codecChange.mediaSessionId.hasValue());
    QVERIFY(!codecChange.keyframeInterval.hasValue());

    const SessionUpdate interval = SessionUpdate::fromJson(parse(R"({"keyframe_interval":5000})"));
    QCOMPARE(interval.keyframeInterval.get(), 5000);
    QVERIFY(!interval.videoCodec.hasValue());
}

// the official client's key order (docs/video-re/re-main-gateway.md)
void TestVoicePayloads::voiceStateUpdateSaysWhetherTheCameraIsOn()
{
    using Discord::voiceStateUpdate;
    const Core::Snowflake guild(100);
    const Core::Snowflake channel(200);

    QCOMPARE(voiceStateUpdate(guild, channel, false, false, true).toJson().toBytes(),
             QByteArray(R"({"op":4,"d":{"guild_id":"100","channel_id":"200","self_mute":false,"self_deaf":false,"self_video":true}})"));
    QCOMPARE(voiceStateUpdate(guild, channel, true, true, false).toJson().toBytes(),
             QByteArray(R"({"op":4,"d":{"guild_id":"100","channel_id":"200","self_mute":true,"self_deaf":true,"self_video":false}})"));
}

void TestVoicePayloads::voiceStateUpdateForAPrivateCallAndForLeaving()
{
    using Discord::voiceStateUpdate;

    QCOMPARE(voiceStateUpdate(Core::Snowflake::Invalid, Core::Snowflake(200), false, false, true).toJson().toBytes(),
             QByteArray(R"({"op":4,"d":{"guild_id":null,"channel_id":"200","self_mute":false,"self_deaf":false,"self_video":true}})"));
    QCOMPARE(voiceStateUpdate(Core::Snowflake(100), Core::Snowflake::Invalid, false, false, false).toJson().toBytes(),
             QByteArray(R"({"op":4,"d":{"guild_id":"100","channel_id":null,"self_mute":false,"self_deaf":false,"self_video":false}})"));
}

void TestVoicePayloads::voiceStateUpdateKeyOrderWithFlags()
{
    Discord::UpdateVoiceState update = Discord::voiceStateUpdate(Core::Snowflake(100), Core::Snowflake(200), false, false, true);
    update.flags = Discord::VoiceFlags(Discord::VoiceFlag::ALLOW_VOICE_RECORDINGS);

    QCOMPARE(update.toJson().toBytes(),
             QByteArray(R"({"op":4,"d":{"guild_id":"100","channel_id":"200","self_mute":false,"self_deaf":false,"self_video":true,"flags":2}})"));
}

QTEST_GUILESS_MAIN(TestVoicePayloads)
#include "tst_VoicePayloads.moc"
