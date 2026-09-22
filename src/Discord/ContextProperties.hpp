#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QList>
#include <QString>

#include <utility>

namespace Acheron {
namespace Discord {

class ContextProperties
{
public:
    static ContextProperties empty();
    static ContextProperties location(const QString &location);

    ContextProperties &add(const QString &key, const QJsonValue &value);

    QByteArray toHeaderValue() const;

private:
    QList<std::pair<QString, QJsonValue>> entries;
};

} // namespace Discord
} // namespace Acheron
