#include "ContextProperties.hpp"

#include <QJsonArray>
#include <QJsonDocument>

namespace Acheron {
namespace Discord {

namespace {

QByteArray jsonLiteral(const QJsonValue &value)
{
    QByteArray array = QJsonDocument(QJsonArray{ value }).toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);
}

} // namespace

ContextProperties ContextProperties::empty()
{
    return {};
}

ContextProperties ContextProperties::location(const QString &location)
{
    return ContextProperties().add("location", location);
}

ContextProperties &ContextProperties::add(const QString &key, const QJsonValue &value)
{
    entries.append({ key, value });
    return *this;
}

QByteArray ContextProperties::toHeaderValue() const
{
    QByteArray json = "{";
    for (int i = 0; i < entries.size(); i++) {
        if (i > 0)
            json += ',';
        json += jsonLiteral(entries[i].first) + ':' + jsonLiteral(entries[i].second);
    }
    json += '}';
    return json.toBase64();
}

} // namespace Discord
} // namespace Acheron
