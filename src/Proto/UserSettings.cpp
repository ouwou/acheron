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

PreloadedUserSettings PreloadedUserSettings::fromProto(ProtoReader &reader)
{
    PreloadedUserSettings settings;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
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

} // namespace Proto
} // namespace Acheron
