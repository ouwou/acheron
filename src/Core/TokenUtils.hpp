#pragma once

#include <QString>
#include <QByteArray>

#include "Snowflake.hpp"

namespace Acheron {
namespace Core {
namespace TokenUtils {

static Snowflake getIdAndCheckToken(const QString &token)
{
    auto parts = token.split('.');
    if (parts.size() < 3)
        return Snowflake::Invalid;

    QByteArray decoded = QByteArray::fromBase64(parts[0].toUtf8());

    bool ok;
    Snowflake id = decoded.toULongLong(&ok);

    if (!ok) {
        // spacebar
        QByteArray decoded = QByteArray::fromBase64(parts[1].toUtf8());
        QJsonDocument doc = QJsonDocument::fromJson(decoded);
        id = doc.object()["id"].toVariant().toULongLong(&ok);
    }

    return ok ? id : Snowflake::Invalid;
}

} // namespace TokenUtils
} // namespace Core
} // namespace Acheron
