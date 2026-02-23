#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "Core/JsonUtils.hpp"
#include "Core/Snowflake.hpp"
#include "VoiceEnums.hpp"

namespace Acheron {
namespace Discord {
namespace AV {

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

struct VoiceReady : Core::JsonUtils::JsonObject
{
    Field<quint32> ssrc;
    Field<QString> ip;
    Field<int> port;
    Field<QStringList> modes;

    static VoiceReady fromJson(const QJsonObject &obj)
    {
        VoiceReady ready;
        get(obj, "ssrc", ready.ssrc);
        get(obj, "ip", ready.ip);
        get(obj, "port", ready.port);
        get(obj, "modes", ready.modes);
        return ready;
    }
};

struct Codec : Core::JsonUtils::JsonObject
{
    Field<QString> name;
    Field<int> payloadType;
    Field<int> priority;
    Field<QString> type;
    Field<int, true> rtxPayloadType;
    Field<bool, true> encode;
    Field<bool, true> decode;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        insert(obj, "name", name);
        insert(obj, "payload_type", payloadType);
        insert(obj, "priority", priority);
        insert(obj, "type", type);
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

    QJsonObject toJson() const
    {
        QJsonObject data;
        insert(data, "address", address);
        insert(data, "port", port);
        insert(data, "mode", mode);

        QJsonObject obj;
        insert(obj, "protocol", protocol);
        obj["data"] = data;
        insert(obj, "address", address);
        insert(obj, "port", port);
        insert(obj, "mode", mode);
        insert(obj, "codecs", codecs);
        return obj;
    }
};

struct SessionDescription : Core::JsonUtils::JsonObject
{
    Field<QString> mode;
    Field<QByteArray> secretKey;
    Field<int, true> daveProtocolVersion;

    static SessionDescription fromJson(const QJsonObject &obj)
    {
        SessionDescription desc;
        get(obj, "mode", desc.mode);
        get(obj, "secret_key", desc.secretKey);
        get(obj, "dave_protocol_version", desc.daveProtocolVersion);
        return desc;
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

    QJsonObject toJson() const
    {
        QJsonObject obj;
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

    static ClientConnectData fromJson(const QJsonObject &obj)
    {
        ClientConnectData data;
        get(obj, "user_id", data.userId);
        get(obj, "audio_ssrc", data.audioSsrc);
        get(obj, "video_ssrc", data.videoSsrc);
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

    QJsonObject toJson() const
    {
        QJsonObject obj;
        obj["channel_id"] = QString::number(channelId.get());
        obj["server_id"] = QString::number(serverId.get());
        obj["session_id"] = sessionId.get();
        obj["token"] = token.get();
        obj["user_id"] = QString::number(userId.get());
        obj["max_dave_protocol_version"] = maxDaveProtocolVersion.get();
        return obj;
    }
};

struct VoiceResumeData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> serverId;
    Field<QString> sessionId;
    Field<QString> token;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        obj["server_id"] = QString::number(serverId.get());
        insert(obj, "session_id", sessionId);
        insert(obj, "token", token);
        return obj;
    }
};

} // namespace AV
} // namespace Discord
} // namespace Acheron
