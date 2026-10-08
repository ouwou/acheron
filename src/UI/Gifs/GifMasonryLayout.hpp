#pragma once

#include <QList>
#include <QRect>
#include <QSize>

namespace Acheron {
namespace UI {
namespace GifMasonry {

inline constexpr int FewestColumns = 2;
inline constexpr int MostColumns = 6;
inline constexpr int RoomiestGutter = 12;
inline constexpr int TightestGutter = 4;
inline constexpr int DenseStillWidth = 128;
inline constexpr int RoomyStillWidth = 256;

struct Arrangement
{
    QList<QRect> tiles;
    int contentHeight = 0;
    int gutter = RoomiestGutter;
};

[[nodiscard]] int gutterFor(int columns);
[[nodiscard]] QSize stillSizeFor(const QSize &naturalSize, int tileWidth);
[[nodiscard]] Arrangement arrange(const QList<QSize> &naturalSizes, int availableWidth, int columns);

} // namespace GifMasonry
} // namespace UI
} // namespace Acheron
