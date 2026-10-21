#pragma once

#include <QFrame>
#include <QPointer>
#include <QUrl>

#include "Core/Snowflake.hpp"

class QLabel;

namespace Acheron {
namespace Core {
class ImageManager;
class StreamPreviews;
} // namespace Core

namespace UI {

class StreamPreviewPopup : public QFrame
{
    Q_OBJECT
public:
    explicit StreamPreviewPopup(Core::ImageManager *imageManager, QWidget *parent = nullptr);

    void showFor(Core::StreamPreviews *previews, Core::Snowflake accountId, const QString &streamKey, const QRect &rowOnScreen);
    void dismiss();

private:
    void showThumbnail(const QUrl &url);
    void showMessage(const QString &message);
    void placeBeside(const QRect &rowOnScreen);

    Core::ImageManager *imageManager;
    QPointer<Core::StreamPreviews> previews;
    QMetaObject::Connection previewChangedConnection;
    Core::Snowflake accountId;
    QString streamKey;
    QUrl thumbnailUrl;

    QLabel *thumbnail;

    static constexpr QSize THUMBNAIL_SIZE = QSize(240, 135);
};

} // namespace UI
} // namespace Acheron
