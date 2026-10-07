#pragma once

#include <QCache>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QRegion>
#include <QUrl>

#include "Core/ImageManager.hpp"
#include "Core/Snowflake.hpp"

class QWidget;

namespace Acheron {
namespace UI {

class PaintedImages : public QObject
{
    Q_OBJECT
public:
    static constexpr int DefaultBudgetKiB = 1024;

    explicit PaintedImages(QWidget *paintedWidget, int budgetKiB = DefaultBudgetKiB);

    void setSource(Core::ImageManager *manager, Core::Snowflake account);
    [[nodiscard]] Core::Snowflake accountId() const { return account; }

    [[nodiscard]] QPixmap pixmap(const QUrl &url, int px, const QRect &paintedRect);
    void clear();

private:
    void onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);
    void onImageUnavailable(const QUrl &url, const QSize &size);
    void remember(const Core::ImageRequestKey &key, const QPixmap &pixmap);

    QWidget *widget;
    Core::ImageManager *imageManager = nullptr;
    Core::Snowflake account;
    QMetaObject::Connection fetchedConnection;
    QMetaObject::Connection unavailableConnection;

    QCache<Core::ImageRequestKey, QPixmap> loaded;
    QHash<Core::ImageRequestKey, QRegion> awaited;
};

} // namespace UI
} // namespace Acheron
