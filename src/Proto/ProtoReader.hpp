#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <optional>

namespace Acheron {
namespace Proto {

enum class WireType : uint8_t {
    VARINT = 0,
    FIXED64 = 1,
    LENGTH_DELIMITED = 2,
    START_GROUP = 3,
    END_GROUP = 4,
    FIXED32 = 5
};

struct Tag
{
    uint32_t fieldNumber;
    WireType wireType;
};

class ProtoReader
{
public:
    explicit ProtoReader(const QByteArray &data);

    bool readTag(Tag &tag);
    bool atEnd() const;

    bool readVarint(uint64_t &value);
    bool readFixed64(uint64_t &value);
    bool readLengthDelimited(QByteArray &value);
    bool readFixed32(uint32_t &value);

    bool skipField(WireType wireType);

    size_t position() const { return pos; }
    size_t remaining() const { return data.size() - pos; }

private:
    const QByteArray data;
    size_t pos = 0;

    bool readByte(uint8_t &byte);
    bool readBytes(void *dest, size_t count);
};

QString readString(ProtoReader &reader);
std::optional<int64_t> readInt64Value(ProtoReader &reader);
std::optional<QString> readStringValue(ProtoReader &reader);
std::optional<uint64_t> readUInt64Value(ProtoReader &reader);

} // namespace Proto
} // namespace Acheron
