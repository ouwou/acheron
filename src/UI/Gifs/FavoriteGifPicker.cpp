#include "UI/Gifs/FavoriteGifPicker.hpp"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include "Core/Theme/Icons.hpp"
#include "UI/Gifs/FavoriteGifGrid.hpp"
#include "UI/Gifs/GifMasonryLayout.hpp"

namespace Acheron {
namespace UI {

namespace {

constexpr int OuterMargin = 8;
constexpr int PreviewSizeSliderWidth = 72;
constexpr auto PreviewColumnsKey = "gif_picker/columns";

} // namespace

FavoriteGifPicker::FavoriteGifPicker(Core::ImageManager *imageManager, QWidget *parent) : QWidget(parent), imageManager(imageManager)
{
    search = new QLineEdit(this);
    search->setPlaceholderText(tr("Search Favorites"));
    search->setClearButtonEnabled(true);
    search->addAction(Core::Theme::Icons::icon(Core::Theme::Icons::Name::Search, Core::Theme::Token::PlaceholderText), QLineEdit::LeadingPosition);

    grid = new FavoriteGifGrid(this);

    const int savedColumns = qBound(GifMasonry::FewestColumns, QSettings().value(PreviewColumnsKey, GifMasonry::FewestColumns).toInt(), GifMasonry::MostColumns);
    previewSize = new QSlider(Qt::Horizontal, this);
    previewSize->setRange(0, GifMasonry::MostColumns - GifMasonry::FewestColumns);
    previewSize->setPageStep(1);
    previewSize->setValue(GifMasonry::MostColumns - savedColumns);
    previewSize->setFixedWidth(PreviewSizeSliderWidth);
    previewSize->setFocusPolicy(Qt::NoFocus);
    previewSize->setToolTip(tr("Preview size"));
    grid->setColumns(savedColumns);

    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(OuterMargin, OuterMargin, OuterMargin, OuterMargin);
    searchRow->setSpacing(OuterMargin);
    searchRow->addWidget(search);
    searchRow->addWidget(previewSize);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(searchRow);
    layout->addWidget(grid, 1);

    connect(search, &QLineEdit::textChanged, this, &FavoriteGifPicker::showMatchingFavorites);
    connect(previewSize, &QSlider::valueChanged, this, &FavoriteGifPicker::onPreviewSizeChanged);
    connect(grid, &FavoriteGifGrid::gifChosen, this, &FavoriteGifPicker::gifPicked);
}

GifPlayback *FavoriteGifPicker::gifPlayback() const
{
    return grid->gifPlayback();
}

void FavoriteGifPicker::onPreviewSizeChanged(int stepsAboveSmallest)
{
    const int columns = GifMasonry::MostColumns - stepsAboveSmallest;
    QSettings().setValue(PreviewColumnsKey, columns);
    grid->setColumns(columns);
}

void FavoriteGifPicker::prepareFor(Core::FavoriteGifs *favoriteGifs, Core::Snowflake accountId)
{
    if (favorites != favoriteGifs) {
        if (favorites)
            disconnect(favorites, nullptr, this, nullptr);
        favorites = favoriteGifs;
        if (favorites)
            connect(favorites, &Core::FavoriteGifs::changed, this, &FavoriteGifPicker::showMatchingFavorites);
    }

    grid->setSources(imageManager, accountId, favoriteGifs);

    const QSignalBlocker blocker(search);
    search->clear();
    showMatchingFavorites();
}

void FavoriteGifPicker::showMatchingFavorites()
{
    const QList<Proto::FavoriteGif> all = favorites ? favorites->newestFirst() : QList<Proto::FavoriteGif>();
    const QString query = search->text().trimmed();

    QList<Proto::FavoriteGif> matching;
    for (const Proto::FavoriteGif &gif : all) {
        if (query.isEmpty() || Core::FavoriteGifRules::matchesSearch(gif.url, query))
            matching.append(gif);
    }

    if (!favorites || !favorites->isLoaded())
        grid->setEmptyText(tr("Loading your favorites..."));
    else if (all.isEmpty())
        grid->setEmptyText(tr("Click the star in the corner of a GIF to favorite it.\nFavorites will show up here!"));
    else
        grid->setEmptyText(tr("No favorite GIFs match your search."));

    grid->setGifs(matching);
}

} // namespace UI
} // namespace Acheron
