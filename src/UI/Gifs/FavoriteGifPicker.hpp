#pragma once

#include <QPointer>
#include <QWidget>

#include "Core/Gifs/FavoriteGifs.hpp"
#include "Core/Snowflake.hpp"

class QLineEdit;
class QSlider;

namespace Acheron {

namespace Core {
class ImageManager;
}

namespace UI {

class FavoriteGifGrid;
class GifPlayback;

class FavoriteGifPicker : public QWidget
{
    Q_OBJECT
public:
    explicit FavoriteGifPicker(Core::ImageManager *imageManager, QWidget *parent = nullptr);

    void prepareFor(Core::FavoriteGifs *favoriteGifs, Core::Snowflake accountId);
    [[nodiscard]] QLineEdit *searchField() const { return search; }
    [[nodiscard]] GifPlayback *gifPlayback() const;

signals:
    void gifPicked(const QString &url);

private:
    void showMatchingFavorites();
    void onPreviewSizeChanged(int stepsAboveSmallest);

    Core::ImageManager *imageManager;
    QLineEdit *search;
    QSlider *previewSize;
    FavoriteGifGrid *grid;
    QPointer<Core::FavoriteGifs> favorites;
};

} // namespace UI
} // namespace Acheron
