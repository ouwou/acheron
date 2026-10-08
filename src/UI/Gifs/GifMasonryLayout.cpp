#include "UI/Gifs/GifMasonryLayout.hpp"

#include <QtGlobal>

#include <algorithm>
#include <vector>

namespace Acheron {
namespace UI {
namespace GifMasonry {

int gutterFor(int columns)
{
    return std::clamp(FewestColumns * RoomiestGutter / qMax(1, columns), TightestGutter, RoomiestGutter);
}

QSize stillSizeFor(const QSize &naturalSize, int tileWidth)
{
    const int width = tileWidth <= DenseStillWidth ? DenseStillWidth : RoomyStillWidth;
    const bool sized = naturalSize.width() > 0 && naturalSize.height() > 0;
    return QSize(width, sized ? qMax(1, qRound(width * double(naturalSize.height()) / naturalSize.width())) : width);
}

Arrangement arrange(const QList<QSize> &naturalSizes, int availableWidth, int requestedColumns)
{
    const int columns = qMax(1, requestedColumns);
    const int gutter = gutterFor(columns);
    const int columnWidth = qMax(1, (availableWidth - 2 * gutter - gutter * (columns - 1)) / columns);

    Arrangement arrangement;
    arrangement.gutter = gutter;
    arrangement.tiles.reserve(naturalSizes.size());
    std::vector<int> columnBottoms(size_t(columns), gutter);

    for (const QSize &natural : naturalSizes) {
        const bool sized = natural.width() > 0 && natural.height() > 0;
        const int height = sized ? qMax(1, qRound(columnWidth * double(natural.height()) / natural.width())) : columnWidth;

        const auto shortest = std::min_element(columnBottoms.begin(), columnBottoms.end());
        const int column = int(shortest - columnBottoms.begin());
        arrangement.tiles.append(QRect(gutter + column * (columnWidth + gutter), *shortest, columnWidth, height));
        *shortest += height + gutter;
    }

    arrangement.contentHeight = *std::max_element(columnBottoms.begin(), columnBottoms.end());
    return arrangement;
}

} // namespace GifMasonry
} // namespace UI
} // namespace Acheron
