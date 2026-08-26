#include "FrecencySettings.hpp"

#include "ProtoReader.hpp"
#include "ProtoWriter.hpp"

namespace Acheron {
namespace Proto {

namespace {

std::optional<uint64_t> readVarintField(ProtoReader &reader, WireType wireType)
{
    if (wireType != WireType::VARINT) {
        reader.skipField(wireType);
        return std::nullopt;
    }

    uint64_t value;
    if (!reader.readVarint(value))
        return std::nullopt;
    return value;
}

int32_t toInt32(uint64_t value)
{
    return static_cast<int32_t>(static_cast<uint32_t>(value));
}

/* 
FrecencyItem
{
    uint32 total_uses = 1;
    repeated uint64 recent_uses = 2;
    int32 frecency = 3;
    int32 score = 4;
}
*/
QByteArray writeFrecencyItem(const FrecencyItem &item)
{
    ProtoWriter writer;
    if (item.totalUses)
        writer.writeVarint(1, item.totalUses);
    writer.writePackedVarints(2, item.recentUses);
    if (item.frecency)
        writer.writeInt32(3, item.frecency);
    if (item.score)
        writer.writeInt32(4, item.score);
    return writer.bytes();
}

/*
EmojiFrecency
{
    map<string, FrecencyItem> emojis = 1;
}
*/
QByteArray writeEmojiFrecency(const EmojiFrecency &frecency)
{
    ProtoWriter writer;
    for (auto it = frecency.emojis.constBegin(); it != frecency.emojis.constEnd(); ++it) {
        ProtoWriter entry;
        entry.writeString(1, it.key());
        entry.writeBytes(2, writeFrecencyItem(it.value()));
        writer.writeBytes(1, entry.bytes());
    }
    return writer.bytes();
}

void readEmojiEntry(ProtoReader &reader, EmojiFrecency &frecency)
{
    QString key;
    FrecencyItem item;
    Tag tag;

    while (reader.readTag(tag)) {
        if (tag.wireType != WireType::LENGTH_DELIMITED) {
            reader.skipField(tag.wireType);
            continue;
        }

        if (tag.fieldNumber == 1) {
            key = readString(reader);
            continue;
        }

        QByteArray nested;
        if (!reader.readLengthDelimited(nested))
            break;
        if (tag.fieldNumber == 2) {
            ProtoReader nestedReader(nested);
            item = FrecencyItem::fromProto(nestedReader);
        }
    }

    if (!key.isEmpty())
        frecency.emojis.insert(key, item);
}

/*
Versions
{
    uint32 client_version = 1;
    uint32 server_version = 2;
    uint32 data_version = 3;
}
*/
std::optional<uint32_t> readDataVersion(ProtoReader &reader)
{
    std::optional<uint32_t> dataVersion;
    Tag tag;

    while (reader.readTag(tag)) {
        const std::optional<uint64_t> value = readVarintField(reader, tag.wireType);
        if (value && tag.fieldNumber == 3)
            dataVersion = static_cast<uint32_t>(*value);
    }

    return dataVersion;
}

} // namespace

FrecencyItem FrecencyItem::fromProto(ProtoReader &reader)
{
    FrecencyItem item;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // uint32 total_uses = 1
        case 1:
            if (const std::optional<uint64_t> value = readVarintField(reader, tag.wireType))
                item.totalUses = static_cast<uint32_t>(*value);
            break;
        // repeated uint64 recent_uses = 2 [packed], but unpacked is valid proto too
        case 2:
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                QByteArray packed;
                if (reader.readLengthDelimited(packed)) {
                    ProtoReader packedReader(packed);
                    uint64_t value;
                    while (!packedReader.atEnd() && packedReader.readVarint(value))
                        item.recentUses.append(value);
                }
            } else if (const std::optional<uint64_t> value =
                               readVarintField(reader, tag.wireType)) {
                item.recentUses.append(*value);
            }
            break;
        // int32 frecency = 3
        case 3:
            if (const std::optional<uint64_t> value = readVarintField(reader, tag.wireType))
                item.frecency = toInt32(*value);
            break;
        // int32 score = 4
        case 4:
            if (const std::optional<uint64_t> value = readVarintField(reader, tag.wireType))
                item.score = toInt32(*value);
            break;
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return item;
}

EmojiFrecency EmojiFrecency::fromProto(ProtoReader &reader)
{
    EmojiFrecency frecency;
    Tag tag;

    while (reader.readTag(tag)) {
        // map<string, FrecencyItem> emojis = 1
        if (tag.fieldNumber == 1 && tag.wireType == WireType::LENGTH_DELIMITED) {
            QByteArray nested;
            if (reader.readLengthDelimited(nested)) {
                ProtoReader nestedReader(nested);
                readEmojiEntry(nestedReader, frecency);
            }
        } else {
            reader.skipField(tag.wireType);
        }
    }

    return frecency;
}

FrecencyUserSettings FrecencyUserSettings::fromProto(ProtoReader &reader)
{
    FrecencyUserSettings settings;
    Tag tag;

    while (reader.readTag(tag)) {
        if (tag.wireType != WireType::LENGTH_DELIMITED) {
            reader.skipField(tag.wireType);
            continue;
        }

        QByteArray nested;
        if (!reader.readLengthDelimited(nested))
            break;

        ProtoReader nestedReader(nested);
        switch (tag.fieldNumber) {
        // Versions versions
        case 1:
            settings.dataVersion = readDataVersion(nestedReader);
            break;
        // EmojiFrecency emoji_frecency
        case 6:
            settings.emojiFrecency = EmojiFrecency::fromProto(nestedReader);
            break;
        // EmojiFrecency emoji_reaction_frecency
        case 13:
            settings.emojiReactionFrecency = EmojiFrecency::fromProto(nestedReader);
            break;
        default:
            break;
        }
    }

    return settings;
}

QByteArray FrecencyUserSettings::toProtoPartial() const
{
    ProtoWriter writer;
    if (emojiFrecency)
        writer.writeBytes(6, writeEmojiFrecency(*emojiFrecency));
    if (emojiReactionFrecency)
        writer.writeBytes(13, writeEmojiFrecency(*emojiReactionFrecency));
    return writer.bytes();
}

} // namespace Proto
} // namespace Acheron
