#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <cstdint>

#include "ProtoReader.hpp"

namespace Acheron {
namespace Proto {

class ProtoWriter
{
public:
    void writeVarint(uint32_t fieldNumber, uint64_t value);
    void writeInt32(uint32_t fieldNumber, int32_t value);
    void writeString(uint32_t fieldNumber, const QString &value);
    void writeBytes(uint32_t fieldNumber, const QByteArray &value);
    void writePackedVarints(uint32_t fieldNumber, const QList<uint64_t> &values);

    const QByteArray &bytes() const { return buffer; }

private:
    void writeTag(uint32_t fieldNumber, WireType wireType);
    void writeRawVarint(uint64_t value);

    QByteArray buffer;
};

} // namespace Proto
} // namespace Acheron
