#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

#include "Core/JsonUtils.hpp"
#include "Core/OrderedJson.hpp"
#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "VoiceEnums.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

struct VoiceHello : Core::JsonUtils::JsonObject
{
    Field<int> heartbeatInterval;

    static VoiceHello fromJson(const QJsonObject &obj)
    {
        VoiceHello hello;
        get(obj, "heartbeat_interval", hello.heartbeatInterval);
        return hello;
    }
};

struct StreamResolution : Core::JsonUtils::JsonObject
{
    Field<QString> type;
    Field<int> width;
    Field<int> height;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "type", type);
        insert(obj, "width", width);
        insert(obj, "height", height);
        return obj;
    }

    static StreamResolution fromJson(const QJsonObject &obj)
    {
        StreamResolution resolution;
        get(obj, "type", resolution.type);
        get(obj, "width", resolution.width);
        get(obj, "height", resolution.height);
        return resolution;
    }
};

struct StreamDescription : Core::JsonUtils::JsonObject
{
    Field<QString> type;
    Field<QString> rid;
    Field<quint32, true> ssrc;
    Field<bool, true> active;
    Field<int, true> quality;
    Field<quint32, true> rtxSsrc;
    Field<int, true> maxBitrate;
    Field<int, true> maxFramerate;
    Field<StreamResolution, true> maxResolution;

    [[nodiscard]] quint32 rtxSsrcOrNext() const { return rtxSsrc.hasValue() && rtxSsrc.get() != 0 ? rtxSsrc.get() : ssrc.valueOr(0) + 1; }

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "type", type);
        insert(obj, "rid", rid);
        insert(obj, "ssrc", ssrc);
        insert(obj, "active", active);
        insert(obj, "quality", quality);
        insert(obj, "rtx_ssrc", rtxSsrc);
        insert(obj, "max_bitrate", maxBitrate);
        insert(obj, "max_framerate", maxFramerate);
        insert(obj, "max_resolution", maxResolution);
        return obj;
    }

    static StreamDescription fromJson(const QJsonObject &obj)
    {
        StreamDescription stream;
        get(obj, "type", stream.type);
        get(obj, "rid", stream.rid);
        get(obj, "ssrc", stream.ssrc);
        get(obj, "active", stream.active);
        get(obj, "quality", stream.quality);
        get(obj, "rtx_ssrc", stream.rtxSsrc);
        get(obj, "max_bitrate", stream.maxBitrate);
        get(obj, "max_framerate", stream.maxFramerate);
        get(obj, "max_resolution", stream.maxResolution);
        return stream;
    }
};

struct VoiceReady : Core::JsonUtils::JsonObject
{
    Field<quint32> ssrc;
    Field<QString> ip;
    Field<int> port;
    Field<QStringList> modes;
    Field<QList<StreamDescription>, true> streams;

    static VoiceReady fromJson(const QJsonObject &obj)
    {
        VoiceReady ready;
        get(obj, "ssrc", ready.ssrc);
        get(obj, "ip", ready.ip);
        get(obj, "port", ready.port);
        get(obj, "modes", ready.modes);
        get(obj, "streams", ready.streams);
        return ready;
    }
};

struct Codec : Core::JsonUtils::JsonObject
{
    Field<QString> name;
    Field<QString> type;
    Field<int> priority;
    Field<int> payloadType;
    Field<int, true> rtxPayloadType;
    Field<bool, true> encode;
    Field<bool, true> decode;

    static constexpr int OPUS_PAYLOAD_TYPE = 120;

    static Codec opus()
    {
        Codec codec;
        codec.name = QStringLiteral("opus");
        codec.type = QStringLiteral("audio");
        codec.priority = 1000;
        codec.payloadType = OPUS_PAYLOAD_TYPE;
        return codec;
    }

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "name", name);
        insert(obj, "type", type);
        insert(obj, "priority", priority);
        insert(obj, "payload_type", payloadType);
        insert(obj, "rtx_payload_type", rtxPayloadType);
        insert(obj, "encode", encode);
        insert(obj, "decode", decode);
        return obj;
    }
};

struct SelectProtocolData : Core::JsonUtils::JsonObject
{
    Field<QString> protocol = QStringLiteral("udp");
    Field<QString> address;
    Field<int> port;
    Field<QString> mode;
    Field<QList<Codec>> codecs;
    Field<QString, true> rtcConnectionId;
    Field<QStringList, true> experiments;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson data;
        insert(data, "address", address);
        insert(data, "port", port);
        insert(data, "mode", mode);

        Core::OrderedJson obj;
        insert(obj, "protocol", protocol);
        obj.insert("data", data);
        insert(obj, "address", address);
        insert(obj, "port", port);
        insert(obj, "mode", mode);
        insert(obj, "codecs", codecs);
        insert(obj, "rtc_connection_id", rtcConnectionId);
        insert(obj, "experiments", experiments);
        return obj;
    }
};

// opcode 14; any of these may change mid-session
struct SessionUpdate : Core::JsonUtils::JsonObject
{
    Field<QString, true> audioCodec;
    Field<QString, true> videoCodec;
    Field<QString, true> mediaSessionId;
    Field<int, true> keyframeInterval;

    static SessionUpdate fromJson(const QJsonObject &obj)
    {
        SessionUpdate update;
        update.read(obj);
        return update;
    }

protected:
    void read(const QJsonObject &obj)
    {
        get(obj, "audio_codec", audioCodec);
        get(obj, "video_codec", videoCodec);
        get(obj, "media_session_id", mediaSessionId);
        get(obj, "keyframe_interval", keyframeInterval);
    }
};

struct SessionDescription : SessionUpdate
{
    Field<QString> mode;
    Field<QByteArray> secretKey;
    Field<int, true> daveProtocolVersion;

    static SessionDescription fromJson(const QJsonObject &obj)
    {
        SessionDescription desc;
        desc.read(obj);
        get(obj, "mode", desc.mode);
        get(obj, "secret_key", desc.secretKey);
        get(obj, "dave_protocol_version", desc.daveProtocolVersion);
        return desc;
    }
};

struct LocalVideoState : Core::JsonUtils::JsonObject
{
    Field<quint32> audioSsrc;
    Field<quint32> videoSsrc;
    Field<quint32> rtxSsrc;
    Field<QList<StreamDescription>> streams;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "audio_ssrc", audioSsrc);
        insert(obj, "video_ssrc", videoSsrc);
        insert(obj, "rtx_ssrc", rtxSsrc);
        insert(obj, "streams", streams);
        return obj;
    }
};

struct MediaSinkWants
{
    std::optional<int> any;
    QMap<quint32, int> bySsrc;
    QMap<quint32, int> pixelCounts;

    // the official client builds this as a JS object, which serializes integer-like keys first in
    // ascending order, so the SSRC entries precede `any`
    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        for (auto it = bySsrc.constBegin(); it != bySsrc.constEnd(); ++it)
            obj.insert(QString::number(it.key()), it.value());
        if (any)
            obj.insert("any", *any);
        if (!pixelCounts.isEmpty()) {
            Core::OrderedJson counts;
            for (auto it = pixelCounts.constBegin(); it != pixelCounts.constEnd(); ++it)
                counts.insert(QString::number(it.key()), it.value());
            obj.insert("pixelCounts", counts);
        }
        return obj;
    }

    static MediaSinkWants fromJson(const QJsonObject &obj)
    {
        MediaSinkWants wants;
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            if (it.key() == QLatin1String("any")) {
                wants.any = it.value().toInt();
            } else if (it.key() == QLatin1String("pixelCounts")) {
                const QJsonObject counts = it.value().toObject();
                for (auto count = counts.constBegin(); count != counts.constEnd(); ++count)
                    wants.pixelCounts.insert(count.key().toUInt(), count.value().toInt());
            } else {
                bool isSsrc = false;
                const quint32 ssrc = it.key().toUInt(&isSsrc);
                if (isSsrc)
                    wants.bySsrc.insert(ssrc, it.value().toInt());
            }
        }
        return wants;
    }
};

struct SpeakingData : Core::JsonUtils::JsonObject
{
    Field<int> speaking;
    Field<int> delay;
    Field<quint32> ssrc;
    Field<Core::Snowflake, true> userId;

    static SpeakingData fromJson(const QJsonObject &obj)
    {
        SpeakingData data;
        get(obj, "speaking", data.speaking);
        get(obj, "delay", data.delay);
        get(obj, "ssrc", data.ssrc);
        get(obj, "user_id", data.userId);
        return data;
    }

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "speaking", speaking);
        insert(obj, "delay", delay);
        insert(obj, "ssrc", ssrc);
        return obj;
    }
};

struct ClientConnectData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> userId;
    Field<quint32> audioSsrc;
    Field<quint32> videoSsrc;
    Field<QList<StreamDescription>, true> streams;

    static ClientConnectData fromJson(const QJsonObject &obj)
    {
        ClientConnectData data;
        get(obj, "user_id", data.userId);
        get(obj, "audio_ssrc", data.audioSsrc);
        get(obj, "video_ssrc", data.videoSsrc);
        get(obj, "streams", data.streams);
        return data;
    }
};

struct VoiceIdentifyData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> serverId;
    Field<Core::Snowflake> userId;
    Field<QString> sessionId;
    Field<QString> token;
    Field<int> maxDaveProtocolVersion = 1;
    Field<bool, true> video;
    Field<QList<StreamDescription>, true> streams;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "server_id", serverId);
        insert(obj, "channel_id", channelId);
        insert(obj, "user_id", userId);
        insert(obj, "session_id", sessionId);
        insert(obj, "token", token);
        insert(obj, "max_dave_protocol_version", maxDaveProtocolVersion);
        insert(obj, "video", video);
        insert(obj, "streams", streams);
        return obj;
    }
};

struct ConnectionParams
{
    QString endpoint;
    QString gatewayScheme = QStringLiteral("wss");
    QString token;
    Core::Snowflake serverId;
    Core::Snowflake channelId;
    Core::Snowflake userId;
    QString sessionId;
    Core::ProxyConfig proxy;

    // off, identify leaves video out and the server treats the connection as audio-only
    bool video = false;
    QList<StreamDescription> streams;
    // payload types are positional, after Opus
    QList<Codec> videoCodecs;
    // a stream viewer sends no audio at all, not even the silence a voice connection opens with
    bool sendsAudio = true;
    bool streamContext = false;
};

struct VoiceResumeData : Core::JsonUtils::JsonObject
{
    Field<QString> token;
    Field<QString> sessionId;
    Field<Core::Snowflake> serverId;
    Field<Core::Snowflake> channelId;
    Field<int> seqAck;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "token", token);
        insert(obj, "session_id", sessionId);
        insert(obj, "server_id", serverId);
        insert(obj, "channel_id", channelId);
        insert(obj, "seq_ack", seqAck);
        return obj;
    }
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Discord::Voice::SpeakingData)
