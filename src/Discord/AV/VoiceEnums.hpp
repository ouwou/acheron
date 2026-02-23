#pragma once

#include <QDebug>
#include <QString>
#include <QStringList>

namespace Acheron {
namespace Discord {
namespace AV {

enum class VoiceOpCode {
    IDENTIFY = 0,
    SELECT_PROTOCOL = 1,
    READY = 2,
    HEARTBEAT = 3,
    SESSION_DESCRIPTION = 4,
    SPEAKING = 5,
    HEARTBEAT_ACK = 6,
    RESUME = 7,
    HELLO = 8,
    RESUMED = 9,
    SIGNAL = 10, // deprecated
    RESET = 11, // deprecated
    CLIENT_CONNECT = 11,
    VIDEO = 12,
    CLIENT_DISCONNECT = 13,
    SESSION_UPDATE = 14,
    MEDIA_SINK_WANTS = 15,
    VOICE_BACKEND_VERSION = 16,
    CHANNEL_OPTIONS_UPDATE = 17, // deprecated
    CLIENT_FLAGS = 18,
    SPEED_TEST = 19,
    CLIENT_PLATFORM = 20,

    DAVE_PROTOCOL_PREPARE_TRANSITION = 21,
    DAVE_PROTOCOL_EXECUTE_TRANSITION = 22,
    DAVE_PROTOCOL_READY_FOR_TRANSITION = 23,
    DAVE_PROTOCOL_PREPARE_EPOCH = 24,
    DAVE_MLS_EXTERNAL_SENDER_PACKAGE = 25,
    DAVE_MLS_KEY_PACKAGE = 26,
    DAVE_MLS_PROPOSALS = 27,
    DAVE_MLS_COMMIT_WELCOME = 28,
    DAVE_MLS_ANNOUNCE_COMMIT_TRANSITION = 29,
    DAVE_MLS_WELCOME = 30,
    DAVE_MLS_INVALID_COMMIT_WELCOME = 31,
};

enum class VoiceCloseCode {
    INTERNAL = 0,

    UNKNOWN_OPCODE = 4001,
    FAILED_TO_DECODE = 4002,
    NOT_AUTHENTICATED = 4003,
    AUTHENTICATION_FAILED = 4004,
    ALREADY_AUTHENTICATED = 4005,
    SESSION_NO_LONGER_VALID = 4006,
    SESSION_TIMEOUT = 4009,
    SERVER_NOT_FOUND = 4011,
    UNKNOWN_PROTOCOL = 4012,
    DISCONNECTED = 4014, // dont reconnect
    VOICE_SERVER_CRASHED = 4015,
    UNKNOWN_ENCRYPTION_MODE = 4016,
    BAD_REQUEST = 4020,
    RATE_LIMITED = 4021, // dont reconnect
    DISCONNECTED_ALL = 4022, // dont reconnect
};

inline QDebug operator<<(QDebug dbg, VoiceCloseCode code)
{
    return dbg << static_cast<int>(code);
}

enum class SpeakingFlag {
    MICROPHONE = 1 << 0,
    SOUNDSHARE = 1 << 1,
    PRIORITY = 1 << 2,
};
Q_DECLARE_FLAGS(SpeakingFlags, SpeakingFlag)
Q_DECLARE_OPERATORS_FOR_FLAGS(SpeakingFlags)

enum class EncryptionMode {
    AEAD_AES256_GCM_RTPSIZE,
    AEAD_XCHACHA20_POLY1305_RTPSIZE,
    UNKNOWN,
};

inline QString encryptionModeToString(EncryptionMode mode)
{
    switch (mode) {
    case EncryptionMode::AEAD_AES256_GCM_RTPSIZE:
        return "aead_aes256_gcm_rtpsize";
    case EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE:
        return "aead_xchacha20_poly1305_rtpsize";
    default:
        return "unknown";
    }
}

inline EncryptionMode encryptionModeFromString(const QString &str)
{
    if (str == "aead_aes256_gcm_rtpsize")
        return EncryptionMode::AEAD_AES256_GCM_RTPSIZE;
    if (str == "aead_xchacha20_poly1305_rtpsize")
        return EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE;
    return EncryptionMode::UNKNOWN;
}

static constexpr int VOICE_GATEWAY_VERSION = 9;

} // namespace AV
} // namespace Discord
} // namespace Acheron
