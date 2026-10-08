#pragma once

#include <QAbstractItemView>
#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QTimer>

#include "Core/Gifs/FavoriteGifs.hpp"
#include "Core/Snowflake.hpp"
#include "UI/Gifs/GifMasonryLayout.hpp"

namespace Acheron {

namespace Core {
class ImageManager;
}

namespace UI {

class GifPlayback;
class PaintedImages;

class FavoriteGifModel : public QAbstractListModel
{
    Q_OBJECT
public:
    using QAbstractListModel::QAbstractListModel;

    void setGifs(const QList<Proto::FavoriteGif> &shownGifs);
    [[nodiscard]] const QList<Proto::FavoriteGif> &gifs() const { return shown; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

private:
    QList<Proto::FavoriteGif> shown;
};

class FavoriteGifGrid : public QAbstractItemView
{
    Q_OBJECT
public:
    explicit FavoriteGifGrid(QWidget *parent = nullptr);

    void setSources(Core::ImageManager *imageManager, Core::Snowflake accountId, Core::FavoriteGifs *favorites);
    void setGifs(const QList<Proto::FavoriteGif> &gifs);
    void setColumns(int columnCount);
    void setEmptyText(const QString &text);
    [[nodiscard]] GifPlayback *gifPlayback() const { return playback; }

    QRect visualRect(const QModelIndex &index) const override;
    void scrollTo(const QModelIndex &index, ScrollHint hint = EnsureVisible) override;
    QModelIndex indexAt(const QPoint &viewportPos) const override;

signals:
    void gifChosen(const QString &url);

protected:
    QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) override;
    int horizontalOffset() const override;
    int verticalOffset() const override;
    bool isIndexHidden(const QModelIndex &index) const override;
    void setSelection(const QRect &rect, QItemSelectionModel::SelectionFlags flags) override;
    QRegion visualRegionForSelection(const QItemSelection &selection) const override;

    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void arrangeTiles();
    void paintTile(QPainter &painter, int row, const QRect &rect);
    void paintHostNotPreviewed(QPainter &painter, const Proto::FavoriteGif &gif, const QRect &rect);
    [[nodiscard]] QPixmap stillFor(const Proto::FavoriteGif &gif, const QRect &rect);
    void fetchStillsHeldBack();
    void onStillFetchFinished(bool fetched);

    FavoriteGifModel *gifModel;
    GifPlayback *playback;
    PaintedImages *stills;
    Core::Snowflake account;

    GifMasonry::Arrangement arrangement;
    int columns = GifMasonry::FewestColumns;
    QString emptyText;
    QTimer scrollSettledTimer;
    QTimer retryStillsTimer;
    bool stillsHeldBack = false;
    int pressedRow = -1;
    bool pressedStar = false;
};

} // namespace UI
} // namespace Acheron
