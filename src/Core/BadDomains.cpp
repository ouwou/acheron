#include "BadDomains.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>
#include <cstring>

namespace Acheron {
namespace Core {

namespace {
const QUrl ListUrl(QStringLiteral("https://cdn.discordapp.com/bad-domains/hashes.json"));

BadDomains::Hash hashOf(const QString &domain)
{
    const QByteArray digest = QCryptographicHash::hash(domain.toUtf8(), QCryptographicHash::Sha256);
    BadDomains::Hash hash{};
    std::memcpy(hash.data(), digest.constData(), hash.size());
    return hash;
}
} // namespace

BadDomains *BadDomains::instance()
{
    static BadDomains *list = new BadDomains(QCoreApplication::instance());
    return list;
}

void BadDomains::fetch()
{
    if (fetchStarted)
        return;
    fetchStarted = true;

    // a manager just for this one request, gone once it's answered
    auto *network = new QNetworkAccessManager(this);
    QNetworkRequest request(ListUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, network] {
        if (reply->error() == QNetworkReply::NoError)
            hashes = parseHashes(reply->readAll());
        reply->deleteLater();
        network->deleteLater();
    });
}

std::vector<BadDomains::Hash> BadDomains::parseHashes(const QByteArray &json)
{
    std::vector<Hash> parsed;
    const QJsonArray list = QJsonDocument::fromJson(json).array();
    parsed.reserve(list.size());
    for (const QJsonValue &value : list) {
        const QByteArray raw = QByteArray::fromHex(value.toString().toLatin1());
        if (raw.size() != 32)
            continue;
        Hash hash{};
        std::memcpy(hash.data(), raw.constData(), hash.size());
        parsed.push_back(hash);
    }
    std::sort(parsed.begin(), parsed.end());
    parsed.erase(std::unique(parsed.begin(), parsed.end()), parsed.end());
    parsed.shrink_to_fit();
    return parsed;
}

bool BadDomains::matches(const std::vector<Hash> &sortedHashes, const QString &host)
{
    if (sortedHashes.empty())
        return false;
    QString domain = host.toLower();
    if (domain.endsWith(QLatin1Char('.')))
        domain.chop(1);
    // the host itself, then each domain above it that still has a dot in it
    while (domain.contains(QLatin1Char('.'))) {
        if (std::binary_search(sortedHashes.begin(), sortedHashes.end(), hashOf(domain)))
            return true;
        domain = domain.mid(domain.indexOf(QLatin1Char('.')) + 1);
    }
    return false;
}

bool BadDomains::isHarmful(const QUrl &url) const
{
    return matches(hashes, url.host(QUrl::FullyEncoded)); // punycode, as the list was hashed
}

} // namespace Core
} // namespace Acheron
