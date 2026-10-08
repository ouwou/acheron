#pragma once

#include <QList>
#include <QSize>
#include <QString>
#include <QUrl>

#include "Proto/FrecencySettings.hpp"

namespace Acheron {
namespace Core {

struct FavoriteGifCandidate
{
    QString url;
    QString src;
    QString gifSrc;
    Proto::GifType format = Proto::GifType::Image;
    QSize size;

    [[nodiscard]] bool isValid() const { return !url.isEmpty() && !src.isEmpty(); }
};

namespace FavoriteGifRules {

inline constexpr int OfficialClientMaxSerializedBytes = 762880;
inline constexpr int CountThatHidesTheTooltip = 2;

[[nodiscard]] QString keyFor(const QString &url);
[[nodiscard]] Proto::FavoriteGif stored(const FavoriteGifCandidate &candidate, uint32_t order);
[[nodiscard]] uint32_t orderAfter(const QList<Proto::FavoriteGif> &gifs);
[[nodiscard]] QList<Proto::FavoriteGif> newestFirst(QList<Proto::FavoriteGif> gifs);
[[nodiscard]] bool matchesSearch(const QString &url, const QString &query);

[[nodiscard]] QUrl clipServedByDiscord(const Proto::FavoriteGif &gif);
[[nodiscard]] QUrl stillServedByDiscord(const Proto::FavoriteGif &gif);

} // namespace FavoriteGifRules

} // namespace Core
} // namespace Acheron
