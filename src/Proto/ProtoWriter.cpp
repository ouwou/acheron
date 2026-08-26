#include "ProtoWriter.hpp"

namespace Acheron {
namespace Proto {

void ProtoWriter::writeRawVarint(uint64_t value)
{
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value)
            byte |= 0x80;
        buffer.append(static_cast<char>(byte));
    } while (value);
}

void ProtoWriter::writeTag(uint32_t fieldNumber, WireType wireType)
{
    writeRawVarint((static_cast<uint64_t>(fieldNumber) << 3) | static_cast<uint64_t>(wireType));
}

void ProtoWriter::writeVarint(uint32_t fieldNumber, uint64_t value)
{
    writeTag(fieldNumber, WireType::VARINT);
    writeRawVarint(value);
}

void ProtoWriter::writeInt32(uint32_t fieldNumber, int32_t value)
{
    // protobuf sign-extends negative int32 to 64 bits, giving a 10 byte varint
    writeVarint(fieldNumber, static_cast<uint64_t>(static_cast<int64_t>(value)));
}

void ProtoWriter::writeString(uint32_t fieldNumber, const QString &value)
{
    writeBytes(fieldNumber, value.toUtf8());
}

void ProtoWriter::writeBytes(uint32_t fieldNumber, const QByteArray &value)
{
    writeTag(fieldNumber, WireType::LENGTH_DELIMITED);
    writeRawVarint(static_cast<uint64_t>(value.size()));
    buffer.append(value);
}

void ProtoWriter::writePackedVarints(uint32_t fieldNumber, const QList<uint64_t> &values)
{
    if (values.isEmpty())
        return;

    ProtoWriter packed;
    for (uint64_t value : values)
        packed.writeRawVarint(value);

    writeBytes(fieldNumber, packed.bytes());
}

} // namespace Proto
} // namespace Acheron
