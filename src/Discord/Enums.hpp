#pragma once

#include <cstdint>

#include <QDebug>
#include <QHash>

#include "qt_flags.hpp"

namespace Acheron {
namespace Discord {

constexpr uint32_t BLURPLE = 0x5865F2;

enum class OpCode {
    DISPATCH = 0,
    HEARTBEAT = 1,
    IDENTIFY = 2,
    PRESENCE_UPDATE = 3,
    VOICE_STATE_UPDATE = 4,
    VOICE_SERVER_PING = 5,
    RESUME = 6,
    RECONNECT = 7,
    REQUEST_GUILD_MEMBERS = 8,
    INVALID_SESSION = 9,
    HELLO = 10,
    HEARTBEAT_ACK = 11,
    GUILD_SYNC = 12,
    CALL_CONNECT = 13,
    GUILD_SUBSCRIPTIONS = 14,
    LOBBY_CONNECT = 15,
    LOBBY_DISCONNECT = 16,
    LOBBY_VOICE_STATES = 17,
    STREAM_CREATE = 18,
    STREAM_DELETE = 19,
    STREAM_WATCH = 20,
    STREAM_PING = 21,
    STREAM_SET_PAUSED = 22,
    LFG_SUBSCRIPTIONS = 23,
    REQUEST_GUILD_APPLICATION_COMMANDS = 24,
    EMBEDDED_ACTIVITY_CREATE = 25,
    EMBEDDED_ACTIVITY_DELETE = 26,
    EMBEDDED_ACTIVITY_UPDATE = 27,
    REQUEST_FORUM_UNREADS = 28,
    REMOTE_COMMAND = 29,
    REQUEST_DELETED_ENTITY_IDS = 30,
    REQUEST_SOUNDBOARD_SOUNDS = 31,
    SPEED_TEST_CREATE = 32,
    SPEED_TEST_DELETE = 33,
    REQUEST_LAST_MESSAGES = 34,
    SEARCH_RECENT_MEMBERS = 35,
    REQUEST_CHANNEL_STATUSES = 36,
    GUILD_SUBSCRIPTIONS_BULK = 37,
    GUILD_CHANNELS_RESYNC = 38,
    REQUEST_CHANNEL_MEMBER_COUNT = 39,
    QOS_HEARTBEAT = 40,
    UPDATE_TIME_SPENT_SESSION_ID = 41,
    LOBBY_VOICE_SERVER_PING = 42,
    REQUEST_CHANNEL_INFO = 43,
};

enum class CloseCode {
    INTERNAL = 0,

    UNKNOWN_ERROR = 4000,
    UNKNOWN_OPCODE = 4001,
    DECODE_ERROR = 4002,
    NOT_AUTHENTICATED = 4003,
    AUTHENTICATION_FAILED = 4004,
    ALREADY_AUTHENTICATED = 4005,
    SESSION_NO_LONGER_VALID = 4006,
    INVALID_SEQ = 4007,
    RATE_LIMITED = 4008,
    SESSION_TIMED_OUT = 4009,
    INVALID_SHARD = 4010,
    SHARDING_REQUIRED = 4011,
    INVALID_API_VERSION = 4012,
    INVALID_INTENTS = 4013,
    DISALLOWED_INTENTS = 4014,
    TOO_MANY_SESSIONS = 4015,
    CONNECTION_REQUEST_CANCELED = 4016,
};
inline QDebug operator<<(QDebug dbg, CloseCode code)
{
    return dbg << static_cast<int>(code);
}

enum class Capability : quint32 {
    LAZY_USER_NOTES = 1 << 0,
    NO_AFFINE_USER_IDS = 1 << 1,
    VERSIONED_READ_STATES = 1 << 2,
    VERSIONED_USER_GUILD_SETTINGS = 1 << 3,
    DEDUPE_USER_OBJECTS = 1 << 4,
    PRIORITIZED_READY_PAYLOAD = 1 << 5,
    MULTIPLE_GUILD_EXPERIMENT_POPULATIONS = 1 << 6,
    NON_CHANNEL_READ_STATES = 1 << 7,
    AUTH_TOKEN_REFRESH = 1 << 8,
    USER_SETTINGS_PROTO = 1 << 9,
    CLIENT_STATE_V2 = 1 << 10,
    PASSIVE_GUILD_UPDATE = 1 << 11,
    AUTO_CALL_CONNECT = 1 << 12,
    DEBOUNCE_MESSAGE_REACTIONS = 1 << 13,
    PASSIVE_GUILD_UPDATE_V2 = 1 << 14,
    // 15
    AUTO_LOBBY_CONNECT = 1 << 16,
    UNK17 = 1 << 17,
    UNK18 = 1 << 18,
    UNK19 = 1 << 19,
    UNK20 = 1 << 20,
};
ACHERON_DECLARE_FLAGS(Capabilities, Capability)

// clang-format off
constexpr static auto CURRENT_CAPABILITIES = Capability::LAZY_USER_NOTES |
                                             Capability::VERSIONED_READ_STATES  |
                                             Capability::VERSIONED_USER_GUILD_SETTINGS |
                                             Capability::DEDUPE_USER_OBJECTS  |
                                             Capability::PRIORITIZED_READY_PAYLOAD       |
                                             Capability::MULTIPLE_GUILD_EXPERIMENT_POPULATIONS  |
                                             Capability::NON_CHANNEL_READ_STATES    |
                                             Capability::AUTH_TOKEN_REFRESH  |
                                             Capability::USER_SETTINGS_PROTO |
                                             Capability::CLIENT_STATE_V2  |
                                             Capability::AUTO_CALL_CONNECT |
                                             Capability::DEBOUNCE_MESSAGE_REACTIONS  |
                                             Capability::PASSIVE_GUILD_UPDATE_V2 |
                                             Capability::UNK17  |
                                             Capability::UNK19  |
                                             Capability::UNK20;
// clang-format on

enum class GatewayEvent {
    UNKNOWN,
    READY,
    READY_SUPPLEMENTAL,
    MESSAGE_CREATE,
    MESSAGE_UPDATE,
    MESSAGE_DELETE,
    TYPING_START,
    CHANNEL_CREATE,
    CHANNEL_UPDATE,
    CHANNEL_DELETE,
    THREAD_CREATE,
    THREAD_UPDATE,
    THREAD_DELETE,
    THREAD_LIST_SYNC,
    THREAD_MEMBER_UPDATE,
    THREAD_MEMBERS_UPDATE,
    FORUM_UNREADS,
    GUILD_CREATE,
    GUILD_UPDATE,
    GUILD_DELETE,
    GUILD_MEMBERS_CHUNK,
    GUILD_MEMBER_ADD,
    GUILD_MEMBER_UPDATE,
    GUILD_MEMBER_REMOVE,
    GUILD_ROLE_CREATE,
    GUILD_ROLE_UPDATE,
    GUILD_ROLE_DELETE,
    GUILD_EMOJIS_UPDATE,
    GUILD_STICKERS_UPDATE,
    GUILD_BAN_ADD,
    GUILD_BAN_REMOVE,
    GUILD_PRUNE_UPDATE,
    MESSAGE_ACK,
    MESSAGE_REACTION_ADD,
    MESSAGE_REACTION_ADD_MANY,
    MESSAGE_REACTION_REMOVE,
    MESSAGE_REACTION_REMOVE_ALL,
    MESSAGE_REACTION_REMOVE_EMOJI,
    USER_GUILD_SETTINGS_UPDATE,
    NOTIFICATION_SETTINGS_UPDATE,
    GUILD_MEMBER_LIST_UPDATE,
    VOICE_STATE_UPDATE,
    VOICE_STATE_UPDATE_BATCH,
    VOICE_SERVER_UPDATE,
    CALL_CREATE,
    CALL_UPDATE,
    CALL_DELETE,
    STREAM_CREATE,
    STREAM_SERVER_UPDATE,
    STREAM_UPDATE,
    STREAM_DELETE,
    RELATIONSHIP_ADD,
    RELATIONSHIP_UPDATE,
    RELATIONSHIP_REMOVE,
    USER_NOTE_UPDATE,
    USER_SETTINGS_PROTO_UPDATE,
    PRESENCE_UPDATE,
    PRESENCES_REPLACE,
    SESSIONS_REPLACE,
};

enum class RelationshipType {
    NONE = 0,
    FRIEND = 1,
    BLOCKED = 2,
    INCOMING_REQUEST = 3,
    OUTGOING_REQUEST = 4,
    IMPLICIT = 5,
};

inline GatewayEvent parseGatewayEvent(const QString &event)
{
    static const QHash<QString, GatewayEvent> events = {
        { "READY", GatewayEvent::READY },
        { "READY_SUPPLEMENTAL", GatewayEvent::READY_SUPPLEMENTAL },
        { "MESSAGE_CREATE", GatewayEvent::MESSAGE_CREATE },
        { "MESSAGE_UPDATE", GatewayEvent::MESSAGE_UPDATE },
        { "MESSAGE_DELETE", GatewayEvent::MESSAGE_DELETE },
        { "TYPING_START", GatewayEvent::TYPING_START },
        { "CHANNEL_CREATE", GatewayEvent::CHANNEL_CREATE },
        { "CHANNEL_UPDATE", GatewayEvent::CHANNEL_UPDATE },
        { "CHANNEL_DELETE", GatewayEvent::CHANNEL_DELETE },
        { "THREAD_CREATE", GatewayEvent::THREAD_CREATE },
        { "THREAD_UPDATE", GatewayEvent::THREAD_UPDATE },
        { "THREAD_DELETE", GatewayEvent::THREAD_DELETE },
        { "THREAD_LIST_SYNC", GatewayEvent::THREAD_LIST_SYNC },
        { "THREAD_MEMBER_UPDATE", GatewayEvent::THREAD_MEMBER_UPDATE },
        { "THREAD_MEMBERS_UPDATE", GatewayEvent::THREAD_MEMBERS_UPDATE },
        { "FORUM_UNREADS", GatewayEvent::FORUM_UNREADS },
        { "GUILD_CREATE", GatewayEvent::GUILD_CREATE },
        { "GUILD_UPDATE", GatewayEvent::GUILD_UPDATE },
        { "GUILD_DELETE", GatewayEvent::GUILD_DELETE },
        { "GUILD_MEMBERS_CHUNK", GatewayEvent::GUILD_MEMBERS_CHUNK },
        { "GUILD_MEMBER_ADD", GatewayEvent::GUILD_MEMBER_ADD },
        { "GUILD_MEMBER_UPDATE", GatewayEvent::GUILD_MEMBER_UPDATE },
        { "GUILD_MEMBER_REMOVE", GatewayEvent::GUILD_MEMBER_REMOVE },
        { "GUILD_ROLE_CREATE", GatewayEvent::GUILD_ROLE_CREATE },
        { "GUILD_ROLE_UPDATE", GatewayEvent::GUILD_ROLE_UPDATE },
        { "GUILD_ROLE_DELETE", GatewayEvent::GUILD_ROLE_DELETE },
        { "GUILD_EMOJIS_UPDATE", GatewayEvent::GUILD_EMOJIS_UPDATE },
        { "GUILD_STICKERS_UPDATE", GatewayEvent::GUILD_STICKERS_UPDATE },
        { "GUILD_BAN_ADD", GatewayEvent::GUILD_BAN_ADD },
        { "GUILD_BAN_REMOVE", GatewayEvent::GUILD_BAN_REMOVE },
        { "GUILD_PRUNE_UPDATE", GatewayEvent::GUILD_PRUNE_UPDATE },
        { "MESSAGE_ACK", GatewayEvent::MESSAGE_ACK },
        { "MESSAGE_REACTION_ADD", GatewayEvent::MESSAGE_REACTION_ADD },
        { "MESSAGE_REACTION_ADD_MANY", GatewayEvent::MESSAGE_REACTION_ADD_MANY },
        { "MESSAGE_REACTION_REMOVE", GatewayEvent::MESSAGE_REACTION_REMOVE },
        { "MESSAGE_REACTION_REMOVE_ALL", GatewayEvent::MESSAGE_REACTION_REMOVE_ALL },
        { "MESSAGE_REACTION_REMOVE_EMOJI", GatewayEvent::MESSAGE_REACTION_REMOVE_EMOJI },
        { "USER_GUILD_SETTINGS_UPDATE", GatewayEvent::USER_GUILD_SETTINGS_UPDATE },
        { "NOTIFICATION_SETTINGS_UPDATE", GatewayEvent::NOTIFICATION_SETTINGS_UPDATE },
        { "GUILD_MEMBER_LIST_UPDATE", GatewayEvent::GUILD_MEMBER_LIST_UPDATE },
        { "VOICE_STATE_UPDATE", GatewayEvent::VOICE_STATE_UPDATE },
        { "VOICE_STATE_UPDATE_BATCH", GatewayEvent::VOICE_STATE_UPDATE_BATCH },
        { "VOICE_SERVER_UPDATE", GatewayEvent::VOICE_SERVER_UPDATE },
        { "CALL_CREATE", GatewayEvent::CALL_CREATE },
        { "CALL_UPDATE", GatewayEvent::CALL_UPDATE },
        { "CALL_DELETE", GatewayEvent::CALL_DELETE },
        { "STREAM_CREATE", GatewayEvent::STREAM_CREATE },
        { "STREAM_SERVER_UPDATE", GatewayEvent::STREAM_SERVER_UPDATE },
        { "STREAM_UPDATE", GatewayEvent::STREAM_UPDATE },
        { "STREAM_DELETE", GatewayEvent::STREAM_DELETE },
        { "RELATIONSHIP_ADD", GatewayEvent::RELATIONSHIP_ADD },
        { "RELATIONSHIP_UPDATE", GatewayEvent::RELATIONSHIP_UPDATE },
        { "RELATIONSHIP_REMOVE", GatewayEvent::RELATIONSHIP_REMOVE },
        { "USER_NOTE_UPDATE", GatewayEvent::USER_NOTE_UPDATE },
        { "USER_SETTINGS_PROTO_UPDATE", GatewayEvent::USER_SETTINGS_PROTO_UPDATE },
        { "PRESENCE_UPDATE", GatewayEvent::PRESENCE_UPDATE },
        { "PRESENCES_REPLACE", GatewayEvent::PRESENCES_REPLACE },
        { "SESSIONS_REPLACE", GatewayEvent::SESSIONS_REPLACE },
    };

    return events.value(event, GatewayEvent::UNKNOWN);
};

enum class StatusType {
    UNKNOWN,
    ONLINE,
    IDLE,
    DND,
    OFFLINE,
    INVISIBLE,
};

inline StatusType parseStatus(const QString &status)
{
    static const QHash<QString, StatusType> statuses = {
        { "online", StatusType::ONLINE },
        { "idle", StatusType::IDLE },
        { "dnd", StatusType::DND },
        { "offline", StatusType::OFFLINE },
        { "invisible", StatusType::INVISIBLE },
        { "unknown", StatusType::UNKNOWN },
    };

    return statuses.value(status, StatusType::OFFLINE);
}

enum class ActivityType {
    PLAYING = 0,
    STREAMING = 1,
    LISTENING = 2,
    WATCHING = 3,
    CUSTOM = 4,
    COMPETING = 5,
    HANG = 6,
};

// which activity field is shown as the status text
enum class StatusDisplayType {
    NAME = 0,
    STATE = 1,
    DETAILS = 2,
};

enum class ActivityFlag {
    INSTANCE = 1 << 0,
    JOIN = 1 << 1,
    SYNC = 1 << 4,
    PLAY = 1 << 5,
    PARTY_PRIVACY_FRIENDS = 1 << 6,
    PARTY_PRIVACY_VOICE_CHANNEL = 1 << 7,
    EMBEDDED = 1 << 8,
    CONTEXTLESS = 1 << 9,
};
ACHERON_DECLARE_FLAGS(ActivityFlags, ActivityFlag)

enum class ChannelType {
    GUILD_TEXT = 0,
    DM = 1,
    GUILD_VOICE = 2,
    GROUP_DM = 3,
    GUILD_CATEGORY = 4,
    GUILD_NEWS = 5,
    GUILD_STORE = 6,
    GUILD_LFG = 7,
    LFG_GROUP_DM = 8,
    THREAD_ALPHA = 9,
    NEWS_THREAD = 10,
    PUBLIC_THREAD = 11,
    PRIVATE_THREAD = 12,
    GUILD_STAGE_VOICE = 13,
    GUILD_DIRECTORY = 14,
    GUILD_FORUM = 15,
    GUILD_MEDIA = 16,
    LOBBY = 17,
    EPHEMERAL_DM = 18,
};

inline bool isThreadType(ChannelType t)
{
    return t == ChannelType::NEWS_THREAD ||
           t == ChannelType::PUBLIC_THREAD ||
           t == ChannelType::PRIVATE_THREAD;
}

inline bool isVoiceType(ChannelType t)
{
    return t == ChannelType::GUILD_VOICE || t == ChannelType::GUILD_STAGE_VOICE;
}

enum class ChannelFlag {
    GUILD_FEED_REMOVED = 1 << 0,
    PINNED = 1 << 1,
    ACTIVE_CHANNELS_REMOVED = 1 << 2,
    REQUIRE_TAG = 1 << 4,
    IS_SPAM = 1 << 5,
    IS_GUILD_RESOURCE_CHANNEL = 1 << 7,
    CLYDE_AI = 1 << 8,
    IS_SCHEDULED_FOR_DELETION = 1 << 9,
    IS_MEDIA_CHANNEL = 1 << 10,
    SUMMARIES_DISABLED = 1 << 11,
    IS_ROLE_SUBSCRIPTION_TEMPLATE_PREVIEW_CHANNEL = 1 << 13,
    IS_BROADCASTING = 1 << 14,
    HIDE_MEDIA_DOWNLOAD_OPTIONS = 1 << 15,
    IS_JOIN_REQUEST_INTERVIEW_CHANNEL = 1 << 16,
    OBFUSCATED = 1 << 17,
    IS_MODERATOR_REPORT_CHANNEL = 1 << 19,
    IS_SPOILER_CHANNEL = 1 << 21,
    IS_GAME_INVITES_CHANNEL = 1 << 22,
    HAS_ONLY_SYSTEM_MESSAGES = 1 << 23,
};
ACHERON_DECLARE_FLAGS(ChannelFlags, ChannelFlag)

// /users/@me/settings-proto/{type}
enum class UserSettingsProtoType {
    PRELOADED = 1,
    FRECENCY = 2,
};

// user
enum class PremiumType {
    NONE = 0,
    TIER_1 = 1, // classic
    TIER_2 = 2, // big boy nitro
    TIER_3 = 3, // basic
};

// guild
enum class PremiumTier {
    NONE = 0,
    TIER_1 = 1,
    TIER_2 = 2,
    TIER_3 = 3,
};

enum class MessageNotificationLevel {
    ALL_MESSAGES = 0,
    ONLY_MENTIONS = 1,
    NO_MESSAGES = 2,
    INHERIT = 3,
};

enum class VerificationLevel {
    NONE = 0,
    LOW = 1,
    MEDIUM = 2,
    HIGH = 3,
    VERY_HIGH = 4,
};

enum class ExplicitContentFilter {
    DISABLED = 0,
    MEMBERS_WITHOUT_ROLES = 1,
    ALL_MEMBERS = 2,
};

enum class MfaLevel {
    NONE = 0,
    ELEVATED = 1,
};

enum class AuditLogAction {
    GUILD_UPDATE = 1,
    CHANNEL_CREATE = 10,
    CHANNEL_UPDATE = 11,
    CHANNEL_DELETE = 12,
    CHANNEL_OVERWRITE_CREATE = 13,
    CHANNEL_OVERWRITE_UPDATE = 14,
    CHANNEL_OVERWRITE_DELETE = 15,
    MEMBER_KICK = 20,
    MEMBER_PRUNE = 21,
    MEMBER_BAN_ADD = 22,
    MEMBER_BAN_REMOVE = 23,
    MEMBER_UPDATE = 24,
    MEMBER_ROLE_UPDATE = 25,
    MEMBER_MOVE = 26,
    MEMBER_DISCONNECT = 27,
    BOT_ADD = 28,
    ROLE_CREATE = 30,
    ROLE_UPDATE = 31,
    ROLE_DELETE = 32,
    INVITE_CREATE = 40,
    INVITE_UPDATE = 41,
    INVITE_DELETE = 42,
    WEBHOOK_CREATE = 50,
    WEBHOOK_UPDATE = 51,
    WEBHOOK_DELETE = 52,
    EMOJI_CREATE = 60,
    EMOJI_UPDATE = 61,
    EMOJI_DELETE = 62,
    MESSAGE_DELETE = 72,
    MESSAGE_BULK_DELETE = 73,
    MESSAGE_PIN = 74,
    MESSAGE_UNPIN = 75,
    INTEGRATION_CREATE = 80,
    INTEGRATION_UPDATE = 81,
    INTEGRATION_DELETE = 82,
    STAGE_INSTANCE_CREATE = 83,
    STAGE_INSTANCE_UPDATE = 84,
    STAGE_INSTANCE_DELETE = 85,
    STICKER_CREATE = 90,
    STICKER_UPDATE = 91,
    STICKER_DELETE = 92,
    GUILD_SCHEDULED_EVENT_CREATE = 100,
    GUILD_SCHEDULED_EVENT_UPDATE = 101,
    GUILD_SCHEDULED_EVENT_DELETE = 102,
    THREAD_CREATE = 110,
    THREAD_UPDATE = 111,
    THREAD_DELETE = 112,
    APPLICATION_COMMAND_PERMISSION_UPDATE = 121,
    SOUNDBOARD_SOUND_CREATE = 130,
    SOUNDBOARD_SOUND_UPDATE = 131,
    SOUNDBOARD_SOUND_DELETE = 132,
    AUTO_MODERATION_RULE_CREATE = 140,
    AUTO_MODERATION_RULE_UPDATE = 141,
    AUTO_MODERATION_RULE_DELETE = 142,
    AUTO_MODERATION_BLOCK_MESSAGE = 143,
    AUTO_MODERATION_FLAG_TO_CHANNEL = 144,
    AUTO_MODERATION_USER_COMMUNICATION_DISABLED = 145,
    AUTO_MODERATION_QUARANTINE_USER = 146,
    CREATOR_MONETIZATION_REQUEST_CREATED = 150,
    CREATOR_MONETIZATION_TERMS_ACCEPTED = 151,
    ONBOARDING_PROMPT_CREATE = 163,
    ONBOARDING_PROMPT_UPDATE = 164,
    ONBOARDING_PROMPT_DELETE = 165,
    ONBOARDING_CREATE = 166,
    ONBOARDING_UPDATE = 167,
    GUILD_HOME_FEATURE_ITEM = 171,
    GUILD_HOME_REMOVE_ITEM = 172,
    HARMFUL_LINKS_BLOCKED_MESSAGE = 180,
    HOME_SETTINGS_CREATE = 190,
    HOME_SETTINGS_UPDATE = 191,
    VOICE_CHANNEL_STATUS_CREATE = 192,
    VOICE_CHANNEL_STATUS_DELETE = 193,
    GUILD_SCHEDULED_EVENT_EXCEPTION_CREATE = 200,
    GUILD_SCHEDULED_EVENT_EXCEPTION_UPDATE = 201,
    GUILD_SCHEDULED_EVENT_EXCEPTION_DELETE = 202,
    GUILD_MEMBER_VERIFICATION_UPDATE = 210,
    GUILD_PROFILE_UPDATE = 211,
    GUILD_MIGRATE_PIN_PERMISSION = 212,
    GUILD_MIGRATE_BYPASS_SLOWMODE_PERMISSION = 213,
};

enum class SystemChannelFlag {
    SUPPRESS_JOIN_NOTIFICATIONS = 1 << 0,
    SUPPRESS_PREMIUM_SUBSCRIPTIONS = 1 << 1,
    SUPPRESS_GUILD_REMINDER_NOTIFICATIONS = 1 << 2,
    SUPPRESS_JOIN_NOTIFICATION_REPLIES = 1 << 3,
    SUPPRESS_ROLE_SUBSCRIPTION_PURCHASE_NOTIFICATIONS = 1 << 4,
    SUPPRESS_ROLE_SUBSCRIPTION_PURCHASE_NOTIFICATION_REPLIES = 1 << 5,
    SUPPRESS_CHANNEL_PROMPT_DEADCHAT = 1 << 7,
};
ACHERON_DECLARE_FLAGS(SystemChannelFlags, SystemChannelFlag)

enum class MessageType {
    DEFAULT = 0,
    RECIPIENT_ADD = 1,
    RECIPIENT_REMOVE = 2,
    CALL = 3,
    CHANNEL_NAME_CHANGE = 4,
    CHANNEL_ICON_CHANGE = 5,
    CHANNEL_PINNED_MESSAGE = 6,
    USER_JOIN = 7,
    PREMIUM_GUILD_SUBSCRIPTION = 8,
    PREMIUM_GUILD_SUBSCRIPTION_TIER_1 = 9,
    PREMIUM_GUILD_SUBSCRIPTION_TIER_2 = 10,
    PREMIUM_GUILD_SUBSCRIPTION_TIER_3 = 11,
    CHANNEL_FOLLOW_ADD = 12,
    GUILD_STREAM = 13,
    GUILD_DISCOVERY_DISQUALIFIED = 14,
    GUILD_DISCOVERY_REQUALIFIED = 15,
    GUILD_DISCOVERY_GRACE_PERIOD_INITIAL_WARNING = 16,
    GUILD_DISCOVERY_GRACE_PERIOD_FINAL_WARNING = 17,
    THREAD_CREATED = 18,
    REPLY = 19,
    CHAT_INPUT_COMMAND = 20,
    THREAD_STARTER_MESSAGE = 21,
    GUILD_INVITE_REMINDER = 22,
    CONTEXT_MENU_COMMAND = 23,
    AUTO_MODERATION_ACTION = 24,
    ROLE_SUBSCRIPTION_PURCHASE = 25,
    INTERACTION_PREMIUM_UPSELL = 26,
    STAGE_START = 27,
    STAGE_END = 28,
    STAGE_SPEAKER = 29,
    STAGE_RAISE_HAND = 30,
    STAGE_TOPIC = 31,
    GUILD_APPLICATION_PREMIUM_SUBSCRIPTION = 32,
    PRIVATE_CHANNEL_INTEGRATION_ADDED = 33,
    PRIVATE_CHANNEL_INTEGRATION_REMOVED = 34,
    PREMIUM_REFERRAL = 35,
    GUILD_INCIDENT_ALERT_MODE_ENABLED = 36,
    GUILD_INCIDENT_ALERT_MODE_DISABLED = 37,
    GUILD_INCIDENT_REPORT_RAID = 38,
    GUILD_INCIDENT_REPORT_FALSE_ALARM = 39,
    GUILD_DEADCHAT_REVIVE_PROMPT = 40,
    CUSTOM_GIFT = 41,
    GUILD_GAMING_STATS_PROMPT = 42,
    POLL = 43,
    PURCHASE_NOTIFICATION = 44,
    VOICE_HANGOUT_INVITE = 45,
    POLL_RESULT = 46,
    CHANGELOG = 47,
    NITRO_NOTIFICATION = 48,
    CHANNEL_LINKED_TO_LOBBY = 49,
    GIFTING_PROMPT = 50,
    IN_GAME_MESSAGE_NUX = 51,
    GUILD_JOIN_REQUEST_ACCEPT_NOTIFICATION = 52,
    GUILD_JOIN_REQUEST_REJECT_NOTIFICATION = 53,
    GUILD_JOIN_REQUEST_WITHDRAWN_NOTIFICATION = 54,
    HD_STREAMING_UPGRADED = 55,
    CHAT_WALLPAPER_SET = 56,
    CHAT_WALLPAPER_REMOVE = 57,
    REPORT_TO_MOD_DELETED_MESSAGE = 58,
    REPORT_TO_MOD_TIMEOUT_USER = 59,
    REPORT_TO_MOD_KICK_USER = 60,
    REPORT_TO_MOD_BAN_USER = 61,
    REPORT_TO_MOD_CLOSED_REPORT = 62,
    EMOJI_ADDED = 63,
};

enum class MessageReferenceType {
    DEFAULT = 0,
    FORWARD = 1,
};

enum class StickerFormatType {
    PNG = 1,
    APNG = 2,
    LOTTIE = 3,
    GIF = 4,
};

enum class MessageFlag {
    CROSSPOSTED = 1 << 0,
    IS_CROSSPOST = 1 << 1,
    SUPPRESS_EMBEDS = 1 << 2,
    SOURCE_MESSAGE_DELETED = 1 << 3,
    URGENT = 1 << 4,
    HAS_THREAD = 1 << 5,
    EPHEMERAL = 1 << 6,
    LOADING = 1 << 7,
    FAILED_TO_MENTION_SOME_ROLES_IN_THREAD = 1 << 8,
    GUILD_FEED_HIDDEN = 1 << 9,
    SHOULD_SHOW_LINK_NOT_DISCORD_WARNING = 1 << 10,
    SUPPRESS_NOTIFICATIONS = 1 << 12,
    IS_VOICE_MESSAGE = 1 << 13,
    HAS_SNAPSHOT = 1 << 14,
    IS_COMPONENTS_V2 = 1 << 15,
    SENT_BY_SOCIAL_LAYER_INTEGRATION = 1 << 16,
};
ACHERON_DECLARE_FLAGS(MessageFlags, MessageFlag)

enum class Permission : quint64 {
    CREATE_INSTANT_INVITE = 1ULL << 0,
    KICK_MEMBERS = 1ULL << 1,
    BAN_MEMBERS = 1ULL << 2,
    ADMINISTRATOR = 1ULL << 3,
    MANAGE_CHANNELS = 1ULL << 4,
    MANAGE_GUILD = 1ULL << 5,
    ADD_REACTIONS = 1ULL << 6,
    VIEW_AUDIT_LOG = 1ULL << 7,
    PRIORITY_SPEAKER = 1ULL << 8,
    STREAM = 1ULL << 9,
    VIEW_CHANNEL = 1ULL << 10,
    SEND_MESSAGES = 1ULL << 11,
    SEND_TTS_MESSAGES = 1ULL << 12,
    MANAGE_MESSAGES = 1ULL << 13,
    EMBED_LINKS = 1ULL << 14,
    ATTACH_FILES = 1ULL << 15,
    READ_MESSAGE_HISTORY = 1ULL << 16,
    MENTION_EVERYONE = 1ULL << 17,
    USE_EXTERNAL_EMOJIS = 1ULL << 18,
    VIEW_GUILD_INSIGHTS = 1ULL << 19,
    CONNECT = 1ULL << 20,
    SPEAK = 1ULL << 21,
    MUTE_MEMBERS = 1ULL << 22,
    DEAFEN_MEMBERS = 1ULL << 23,
    MOVE_MEMBERS = 1ULL << 24,
    USE_VAD = 1ULL << 25,
    CHANGE_NICKNAME = 1ULL << 26,
    MANAGE_NICKNAMES = 1ULL << 27,
    MANAGE_ROLES = 1ULL << 28,
    MANAGE_WEBHOOKS = 1ULL << 29,
    MANAGE_EXPRESSIONS = 1ULL << 30,
    USE_APPLICATION_COMMANDS = 1ULL << 31,
    REQUEST_TO_SPEAK = 1ULL << 32,
    MANAGE_EVENTS = 1ULL << 33,
    MANAGE_THREADS = 1ULL << 34,
    CREATE_PUBLIC_THREADS = 1ULL << 35,
    CREATE_PRIVATE_THREADS = 1ULL << 36,
    USE_EXTERNAL_STICKERS = 1ULL << 37,
    SEND_MESSAGES_IN_THREADS = 1ULL << 38,
    USE_EMBEDDED_ACTIVITIES = 1ULL << 39,
    MODERATE_MEMBERS = 1ULL << 40,
    VIEW_CREATOR_MONETIZATION_ANALYTICS = 1ULL << 41,
    USE_SOUNDBOARD = 1ULL << 42,
    CREATE_EXPRESSIONS = 1ULL << 43,
    CREATE_EVENTS = 1ULL << 44,
    USE_EXTERNAL_SOUNDS = 1ULL << 45,
    SEND_VOICE_MESSAGES = 1ULL << 46,
    USE_CLYDE_AI = 1ULL << 47,
    SET_VOICE_CHANNEL_STATUS = 1ULL << 48,
    SEND_POLLS = 1ULL << 49,
    USE_EXTERNAL_APPS = 1ULL << 50,
    PIN_MESSAGES = 1ULL << 51,
    BYPASS_SLOWMODE = 1ULL << 52,
};
ACHERON_DECLARE_FLAGS(Permissions, Permission)

constexpr Permissions ALL_PERMISSIONS = Permissions::fromInt(0xFFFFFFFFFFFFFFFFULL);
constexpr Permissions NO_PERMISSIONS = Permissions::fromInt(0ULL);

constexpr Permissions MFA_ELEVATED_PERMISSIONS = Permissions::fromInt(quint64(Permission::KICK_MEMBERS) |
                                                                      quint64(Permission::BAN_MEMBERS) |
                                                                      quint64(Permission::ADMINISTRATOR) |
                                                                      quint64(Permission::MANAGE_CHANNELS) |
                                                                      quint64(Permission::MANAGE_GUILD) |
                                                                      quint64(Permission::MANAGE_ROLES) |
                                                                      quint64(Permission::MANAGE_MESSAGES) |
                                                                      quint64(Permission::MANAGE_THREADS) |
                                                                      quint64(Permission::MODERATE_MEMBERS));

enum class ReadStateFlag {
    IS_GUILD_CHANNEL = 1 << 0,
    IS_THREAD = 1 << 1,
    IS_MENTION_LOW_IMPORTANCE = 1 << 2,
};
ACHERON_DECLARE_FLAGS(ReadStateFlags, ReadStateFlag)

enum class NotificationSettingsFlag {
    USE_NEW_NOTIFICATIONS = 1 << 4,
    MENTION_ON_ALL_MESSAGES = 1 << 5,
};

enum class UserGuildSettingsFlag {
    UNREADS_ALL_MESSAGES = 1 << 11,
    UNREADS_ONLY_MENTIONS = 1 << 12,
    OPT_IN_CHANNELS_OFF = 1 << 13,
    OPT_IN_CHANNELS_ON = 1 << 14,
};

enum class ChannelOverrideFlag {
    UNREADS_ONLY_MENTIONS = 1 << 9,
    UNREADS_ALL_MESSAGES = 1 << 10,
    FAVORITED = 1 << 11,
    OPT_IN_ENABLED = 1 << 12,
    NEW_FORUM_THREADS_OFF = 1 << 13,
    NEW_FORUM_THREADS_ON = 1 << 14,
};

enum class AttachmentFlag {
    IS_CLIP = 1 << 0,
    IS_THUMBNAIL = 1 << 1,
    IS_REMIX = 1 << 2,
    IS_SPOILER = 1 << 3,
    CONTAINS_EXPLICIT_MEDIA = 1 << 4,
    IS_ANIMATED = 1 << 5,
    CONTAINS_GORE_CONTENT = 1 << 6,
    CONTAINS_SELF_HARM_CONTENT = 1 << 7,
};
ACHERON_DECLARE_FLAGS(AttachmentFlags, AttachmentFlag)

enum class VoiceFlag {
    CLIPS_ENABLED = 1 << 0,
    ALLOW_VOICE_RECORDINGS = 1 << 1,
    ALLOW_ANY_VIEWER_CLIPS = 1 << 2,
};
ACHERON_DECLARE_FLAGS(VoiceFlags, VoiceFlag)

} // namespace Discord
} // namespace Acheron
