#include "UserSettings.hpp"

#include "ProtoReader.hpp"
#include "Core/Logging.hpp"

namespace Acheron {
namespace Proto {

GuildFolder GuildFolder::fromProto(ProtoReader &reader)
{
    GuildFolder folder;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // repeated fixed64 guild_ids
        case 1: {
            if (tag.wireType == WireType::FIXED64) {
                uint64_t id;
                if (reader.readFixed64(id))
                    folder.guildIds.append(Core::Snowflake(id));
            } else if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray packed;
                if (reader.readLengthDelimited(packed)) {
                    ProtoReader packedReader(packed);
                    uint64_t id;
                    while (!packedReader.atEnd() && packedReader.readFixed64(id))
                        folder.guildIds.append(Core::Snowflake(id));
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // optional google.protobuf.Int64Value id
        case 2: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    folder.id = readInt64Value(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // optional google.protobuf.StringValue name
        case 3: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    folder.name = readStringValue(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // optional google.protobuf.UInt64Value color
        case 4: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    folder.color = readUInt64Value(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return folder;
}

GuildFolders GuildFolders::fromProto(ProtoReader &reader)
{
    GuildFolders guildFolders;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // repeated GuildFolder folders
        case 1: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    guildFolders.folders.append(GuildFolder::fromProto(nestedReader));
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // repeated fixed64 guild_positions
        case 2: {
            if (tag.wireType == WireType::FIXED64) {
                uint64_t id;
                if (reader.readFixed64(id))
                    guildFolders.guildPositions.append(Core::Snowflake(id));
            } else if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray packed;
                if (reader.readLengthDelimited(packed)) {
                    ProtoReader packedReader(packed);
                    uint64_t id;
                    while (!packedReader.atEnd() && packedReader.readFixed64(id))
                        guildFolders.guildPositions.append(Core::Snowflake(id));
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return guildFolders;
}

CustomStatus CustomStatus::fromProto(ProtoReader &reader)
{
    CustomStatus custom;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // string text
        case 1: {
            if (tag.wireType == WireType::LENGTH_DELIMITED)
                custom.text = readString(reader);
            else
                reader.skipField(tag.wireType);
            break;
        }
        // fixed64 emoji_id
        case 2: {
            uint64_t id;
            if (tag.wireType == WireType::FIXED64 && reader.readFixed64(id))
                custom.emojiId = id;
            else
                reader.skipField(tag.wireType);
            break;
        }
        // string emoji_name
        case 3: {
            if (tag.wireType == WireType::LENGTH_DELIMITED)
                custom.emojiName = readString(reader);
            else
                reader.skipField(tag.wireType);
            break;
        }
        // fixed64 expires_at_ms
        case 4: {
            uint64_t value;
            if (tag.wireType == WireType::FIXED64 && reader.readFixed64(value))
                custom.expiresAtMs = value;
            else
                reader.skipField(tag.wireType);
            break;
        }
        // fixed64 created_at_ms
        case 5: {
            uint64_t value;
            if (tag.wireType == WireType::FIXED64 && reader.readFixed64(value))
                custom.createdAtMs = value;
            else
                reader.skipField(tag.wireType);
            break;
        }
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return custom;
}

StatusSettings StatusSettings::fromProto(ProtoReader &reader)
{
    StatusSettings settings;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // optional google.protobuf.StringValue status
        case 1: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    settings.status = readStringValue(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // optional CustomStatus custom_status
        case 2: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    settings.customStatus = CustomStatus::fromProto(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // optional google.protobuf.BoolValue show_current_game
        case 3: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    settings.showCurrentGame = readBoolValue(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // fixed64 status_expires_at_ms
        case 4: {
            uint64_t value;
            if (tag.wireType == WireType::FIXED64 && reader.readFixed64(value))
                settings.statusExpiresAtMs = value;
            else
                reader.skipField(tag.wireType);
            break;
        }
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return settings;
}

PreloadedUserSettings PreloadedUserSettings::fromProto(ProtoReader &reader)
{
    PreloadedUserSettings settings;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // optional StatusSettings status
        case 11: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    settings.status = StatusSettings::fromProto(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // optional GuildFolders guild_folders
        case 14: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    settings.guildFolders = GuildFolders::fromProto(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return settings;
}

PreloadedUserSettings PreloadedUserSettings::fromBase64(const QString &base64)
{
    ProtoReader reader(QByteArray::fromBase64(base64.toUtf8()));
    return fromProto(reader);
}

void StatusSettings::mergeFrom(const StatusSettings &other)
{
    if (other.status.has_value())
        status = other.status;
    if (other.customStatus.has_value())
        customStatus = other.customStatus;
    if (other.showCurrentGame.has_value())
        showCurrentGame = other.showCurrentGame;
    if (other.statusExpiresAtMs != 0)
        statusExpiresAtMs = other.statusExpiresAtMs;
}

void PreloadedUserSettings::mergeFrom(const PreloadedUserSettings &other)
{
    if (other.guildFolders.has_value())
        guildFolders = other.guildFolders;

    if (!other.status.has_value())
        return;

    if (status.has_value())
        status->mergeFrom(other.status.value());
    else
        status = other.status;
}

} // namespace Proto
} // namespace Acheron
