#include "Core/Gifs/FavoriteGifRules.hpp"

#include <QRegularExpression>
#include <QUrlQuery>

#include <algorithm>

#include "Discord/CdnUrls.hpp"

namespace Acheron {
namespace Core {
namespace FavoriteGifRules {

namespace {

bool isAnimatedImageFile(const QString &url)
{
    static const QRegularExpression pattern(QStringLiteral(R"(\.(webp|avif|gif)(\?|$))"), QRegularExpression::CaseInsensitiveOption);
    return pattern.match(url).hasMatch();
}

bool isVideoFile(const QString &url)
{
    static const QRegularExpression pattern(QStringLiteral(R"(\.(mp4|webm)(\?|$))"), QRegularExpression::CaseInsensitiveOption);
    return pattern.match(url).hasMatch();
}

bool isAttachmentOnDiscord(const QUrl &url)
{
    const QString host = url.host();
    const QString path = url.path();
    return (host == QLatin1String("cdn.discordapp.com") || host == QLatin1String("media.discordapp.net")) &&
           (path.startsWith(QLatin1String("/attachments/")) || path.startsWith(QLatin1String("/ephemeral-attachments/")));
}

bool isProxiedExternalMedia(const QUrl &url)
{
    const QString host = url.host();
    return host.startsWith(QLatin1String("images-ext-")) && host.endsWith(QLatin1String(".discordapp.net")) && url.path().startsWith(QLatin1String("/external/"));
}

bool isHostedByDiscord(const QString &src)
{
    const QUrl url(src);
    return !url.scheme().isEmpty() && (isProxiedExternalMedia(url) || isAttachmentOnDiscord(url));
}

QUrlQuery queryWithoutEmptyPairs(const QUrl &url)
{
    auto items = QUrlQuery(url).queryItems(QUrl::FullyEncoded);
    items.erase(std::remove_if(items.begin(), items.end(), [](const auto &item) { return item.first.isEmpty() && item.second.isEmpty(); }), items.end());

    QUrlQuery query;
    query.setQueryItems(items);
    return query;
}

void setQueryItem(QUrlQuery &query, const QString &name, const QString &value)
{
    auto items = query.queryItems(QUrl::FullyEncoded);
    const auto named = [&name](const auto &item) { return item.first == name; };
    const auto existing = std::find_if(items.begin(), items.end(), named);
    if (existing == items.end()) {
        query.addQueryItem(name, value);
        return;
    }

    existing->second = value;
    items.erase(std::remove_if(existing + 1, items.end(), named), items.end());
    query.setQueryItems(items);
}

QUrl withoutQueryItems(QUrl url, std::initializer_list<QLatin1String> names)
{
    QUrlQuery query = queryWithoutEmptyPairs(url);
    for (const QLatin1String name : names)
        query.removeAllQueryItems(name);
    url.setQuery(query);
    return url;
}

QUrl asAnimatedWebp(QUrl url)
{
    const QString path = url.path().toLower();
    const bool webp = path.endsWith(QLatin1String(".webp"));
    const bool needsConversion = path.endsWith(QLatin1String(".avif")) || path.endsWith(QLatin1String(".gif"));
    if (!webp && !needsConversion)
        return url;

    QUrlQuery query = queryWithoutEmptyPairs(url);
    if (needsConversion)
        setQueryItem(query, QStringLiteral("format"), QStringLiteral("webp"));
    setQueryItem(query, QStringLiteral("animated"), QStringLiteral("true"));
    url.setQuery(query);
    return url;
}

QString asAnimatedWebp(const QString &src)
{
    const QUrl url(src);
    if (!url.isValid() || url.scheme().isEmpty())
        return src;

    const QUrl animated = asAnimatedWebp(url);
    return animated == url ? src : animated.toString(QUrl::FullyEncoded);
}

QString withoutSearchNoise(QString text, const QRegularExpression &noise)
{
    return text.toLower().remove(noise);
}

} // namespace

QString keyFor(const QString &url)
{
    const QUrl parsed(url);
    if (!parsed.isValid() || parsed.scheme().isEmpty() || !isAttachmentOnDiscord(parsed))
        return url;

    return withoutQueryItems(parsed, { QLatin1String("ex"), QLatin1String("is"), QLatin1String("hm") }).toString(QUrl::FullyEncoded);
}

Proto::FavoriteGif stored(const FavoriteGifCandidate &candidate, uint32_t order)
{
    const bool srcOnDiscord = isHostedByDiscord(candidate.src);
    const bool hasOtherGifSrc = !candidate.gifSrc.isEmpty() && candidate.gifSrc != candidate.src;
    const bool preferGifSrc = (isVideoFile(candidate.src) && hasOtherGifSrc) || (srcOnDiscord && !candidate.gifSrc.isEmpty());

    QString src = preferGifSrc ? candidate.gifSrc : candidate.src;
    if (srcOnDiscord && isAnimatedImageFile(src))
        src = asAnimatedWebp(src);
    if (src.startsWith(QLatin1String("//")))
        src.prepend(QLatin1String("https:"));

    Proto::FavoriteGif gif;
    gif.url = keyFor(candidate.url);
    gif.src = src;
    gif.format = isAnimatedImageFile(src) ? Proto::GifType::Image : candidate.format;
    gif.width = uint32_t(qMax(0, candidate.size.width()));
    gif.height = uint32_t(qMax(0, candidate.size.height()));
    gif.order = order;
    return gif;
}

uint32_t orderAfter(const QList<Proto::FavoriteGif> &gifs)
{
    uint32_t highest = 0;
    for (const Proto::FavoriteGif &gif : gifs)
        highest = std::max(highest, gif.order);
    return highest + 1;
}

QList<Proto::FavoriteGif> newestFirst(QList<Proto::FavoriteGif> gifs)
{
    std::stable_sort(gifs.begin(), gifs.end(), [](const Proto::FavoriteGif &a, const Proto::FavoriteGif &b) { return a.order < b.order; });
    std::reverse(gifs.begin(), gifs.end());
    return gifs;
}

bool matchesSearch(const QString &url, const QString &query)
{
    static const QRegularExpression separators(QStringLiteral("[-_]"));
    static const QRegularExpression separatorsAndSpaces(QStringLiteral("[-_ ]"));
    return withoutSearchNoise(url, separators).contains(withoutSearchNoise(query, separatorsAndSpaces));
}

QUrl clipServedByDiscord(const Proto::FavoriteGif &gif)
{
    const QUrl url(gif.src);
    return Discord::Cdn::isDiscordAssetUrl(url) ? asAnimatedWebp(url) : QUrl();
}

QUrl stillServedByDiscord(const Proto::FavoriteGif &gif)
{
    const QUrl url(gif.src);
    if (!Discord::Cdn::isDiscordAssetUrl(url))
        return {};
    return withoutQueryItems(url, { QLatin1String("animated"), QLatin1String("format") });
}

} // namespace FavoriteGifRules
} // namespace Core
} // namespace Acheron
