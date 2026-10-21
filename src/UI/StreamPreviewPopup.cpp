#include "StreamPreviewPopup.hpp"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QVBoxLayout>

#include "Core/ImageManager.hpp"
#include "Core/Streams/StreamPreviews.hpp"
#include "Core/Theme/Manager.hpp"
#include "UI/LivePill.hpp"

namespace Acheron {
namespace UI {

StreamPreviewPopup::StreamPreviewPopup(Core::ImageManager *imageManager, QWidget *parent)
    : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus), imageManager(imageManager)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFrameShape(QFrame::StyledPanel);

    auto *title = new QLabel(tr("Streaming Now"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto *livePill = new QLabel(livePillText(), this);
    livePill->setFont(livePillFont(livePill->font()));
    livePill->setStyleSheet(QStringLiteral("QLabel { background: %1; color: white; border-radius: %2px; padding: 0px %3px; }").arg(livePillColour().name()).arg(LIVE_PILL_RADIUS).arg(LIVE_PILL_PADDING));

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->addWidget(title);
    header->addStretch();
    header->addWidget(livePill);

    thumbnail = new QLabel(this);
    thumbnail->setFixedSize(THUMBNAIL_SIZE);
    thumbnail->setAlignment(Qt::AlignCenter);
    thumbnail->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->setSpacing(6);
    layout->addLayout(header);
    layout->addWidget(thumbnail);
}

void StreamPreviewPopup::showFor(Core::StreamPreviews *streamPreviews, Core::Snowflake account, const QString &key, const QRect &rowOnScreen)
{
    disconnect(previewChangedConnection);
    previews = streamPreviews;
    accountId = account;
    streamKey = key;
    thumbnailUrl.clear();

    if (!previews) {
        showMessage(tr("No permission"));
    } else {
        previewChangedConnection = connect(previews, &Core::StreamPreviews::previewChanged, this, [this](const QString &changedKey, const QUrl &url) {
            if (changedKey == streamKey && !url.isEmpty())
                showThumbnail(url);
        });

        const QUrl cached = previews->request(streamKey);
        if (cached.isEmpty())
            showMessage(tr("Stream just started. Get in here!"));
        else
            showThumbnail(cached);
    }

    adjustSize();
    placeBeside(rowOnScreen);
    show();
}

void StreamPreviewPopup::dismiss()
{
    disconnect(previewChangedConnection);
    streamKey.clear();
    thumbnailUrl.clear();
    hide();
}

void StreamPreviewPopup::showMessage(const QString &message)
{
    thumbnail->setPixmap(QPixmap());
    thumbnail->setText(message);
}

void StreamPreviewPopup::showThumbnail(const QUrl &url)
{
    if (url == thumbnailUrl)
        return;
    thumbnailUrl = url;

    // through the account's own route, so a proxied account's thumbnails are proxied
    imageManager->fetch(url, accountId, this, [this, url](const QByteArray &bytes) {
        if (url != thumbnailUrl)
            return;

        QPixmap pixmap;
        if (!pixmap.loadFromData(bytes))
            return;

        const qreal dpr = devicePixelRatioF();
        pixmap = pixmap.scaled(THUMBNAIL_SIZE * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(dpr);
        thumbnail->setText(QString());
        thumbnail->setPixmap(pixmap);
    });
}

void StreamPreviewPopup::placeBeside(const QRect &rowOnScreen)
{
    constexpr int gap = 8;

    QPoint topLeft(rowOnScreen.right() + gap, rowOnScreen.center().y() - height() / 2);

    const QScreen *screen = QGuiApplication::screenAt(rowOnScreen.center());
    if (screen) {
        const QRect available = screen->availableGeometry();
        if (topLeft.x() + width() > available.right())
            topLeft.setX(rowOnScreen.left() - gap - width());
        topLeft.setY(qBound(available.top(), topLeft.y(), available.bottom() - height()));
    }

    move(topLeft);
}

} // namespace UI
} // namespace Acheron
