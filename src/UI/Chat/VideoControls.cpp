#include "UI/Chat/VideoControls.hpp"

#include "Core/Theme/Icons.hpp"
#include "Core/Theme/Manager.hpp"
#include "Core/Video/Player.hpp"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QPaintDevice>
#include <QPainter>
#include <QPolygon>

#include <algorithm>

namespace Acheron {
namespace UI {
namespace VideoControls {

namespace {

constexpr int Padding = 8;
constexpr int Gap = 6;
constexpr int TrackHeight = 4;
constexpr int ButtonSize = 24;
constexpr int GlyphSize = 16;
constexpr int HandleRadius = 6;
constexpr int VolumeTrackLength = 56;
constexpr int VolumePopupPadding = 8;
constexpr int SeekGrabSlack = 7;

constexpr int ScrimAlpha = 190;
constexpr int BadgeAlpha = 180;

QColor trackBackground()
{
    return QColor(255, 255, 255, 70);
}

QColor accent()
{
    return Core::Theme::Manager::instance().color(Core::Theme::Token::Highlight);
}

qreal deviceRatio(const QPainter *painter)
{
    if (painter && painter->device())
        return painter->device()->devicePixelRatioF();
    return 1.0;
}

void drawGlyph(QPainter *painter, const QRect &rect, const QString &name)
{
    const qreal dpr = deviceRatio(painter);
    const QPixmap glyph = Core::Theme::Icons::pixmap(name, GlyphSize, QColor(Qt::white), dpr);
    if (glyph.isNull())
        return;

    const QSize logical = glyph.size() / glyph.devicePixelRatio();
    const QPoint topLeft(rect.center().x() - logical.width() / 2,
                         rect.center().y() - logical.height() / 2);
    painter->drawPixmap(topLeft, glyph);
}

} // namespace

State stateFor(const Core::Video::Player *player, bool volumeExpanded, bool fullscreen)
{
    State state;
    state.fullscreen = fullscreen;

    if (!player)
        return state;

    state.playing = player->isPlaying();
    state.muted = player->isMuted();
    state.volume = player->volume();
    state.positionMs = player->position();
    state.durationMs = player->duration();
    state.bufferedMs = player->bufferedPosition();
    state.volumeExpanded = volumeExpanded;
    return state;
}

bool beginDrag(Core::Video::Player *player, const Layout &layout, const State &state, const QPoint &pos, Drag &drag, const QString &key)
{
    if (!player)
        return false;

    switch (hitTest(layout, pos, state)) {
    case Hit::Seek:
        if (!player->isSeekable())
            return false;
        player->seek(positionForSeekX(layout, pos.x(), player->duration()));
        drag.kind = Drag::Kind::Seek;
        break;

    case Hit::VolumeSlider:
        player->setMuted(false);
        player->setVolume(volumeForSliderY(layout, pos.y()));
        drag.kind = Drag::Kind::Volume;
        break;

    default:
        return false;
    }

    drag.key = key;
    return true;
}

void applyDrag(Core::Video::Player *player, const Layout &layout, const QPoint &pos, const Drag &drag)
{
    if (!player || !drag.active())
        return;

    if (drag.isSeek()) {
        if (player->isSeekable())
            player->seek(positionForSeekX(layout, pos.x(), player->duration()));
    } else {
        player->setVolume(volumeForSliderY(layout, pos.y()));
    }
}

ReleaseResult handleRelease(Core::Video::Player *player, const Layout &layout, const State &state,
                            const QPoint &pos)
{
    if (!player)
        return ReleaseResult::None;

    switch (hitTest(layout, pos, state)) {
    case Hit::Play:
    case Hit::Surface:
        player->togglePlayPause();
        return ReleaseResult::Handled;

    case Hit::Volume:
        player->setMuted(!player->isMuted());
        return ReleaseResult::Handled;

    case Hit::Fullscreen:
        return ReleaseResult::ToggleFullscreen;

    default:
        return ReleaseResult::None;
    }
}

int barHeight()
{
    return Padding + TrackHeight + Gap + ButtonSize + Padding;
}

Layout calculate(const QRect &videoRect, const State &state)
{
    Layout layout;
    layout.video = videoRect;

    if (videoRect.isEmpty())
        return layout;

    const int height = qMin(barHeight(), videoRect.height());
    layout.bar = QRect(videoRect.left(), videoRect.bottom() - height + 1, videoRect.width(), height);

    const int left = layout.bar.left() + Padding;
    const int right = layout.bar.right() - Padding;
    if (right <= left)
        return layout;

    layout.seek = QRect(left, layout.bar.top() + Padding, right - left, TrackHeight);

    const int rowTop = layout.seek.bottom() + Gap;
    layout.play = QRect(left, rowTop, ButtonSize, ButtonSize);
    layout.fullscreen = QRect(right - ButtonSize + 1, rowTop, ButtonSize, ButtonSize);

    const bool roomForVolume = layout.fullscreen.left() - Gap - ButtonSize > layout.play.right() + Gap;
    if (roomForVolume)
        layout.volume = QRect(layout.fullscreen.left() - Gap - ButtonSize, rowTop, ButtonSize, ButtonSize);

    const int trailingLeft = layout.volume.isNull() ? layout.fullscreen.left() : layout.volume.left();

    if (state.volumeExpanded && !layout.volume.isNull()) {
        const int popupHeight = VolumeTrackLength + VolumePopupPadding * 2;
        const int popupBottom = layout.volume.top() - Gap;
        const int popupTop = popupBottom - popupHeight + 1;

        if (popupTop >= videoRect.top()) {
            const int popupLeft = layout.volume.left();
            layout.volumePopup = QRect(popupLeft, popupTop, ButtonSize, popupHeight);

            const int trackLeft = popupLeft + (ButtonSize - TrackHeight) / 2;
            layout.volumeSlider = QRect(trackLeft, popupTop + VolumePopupPadding, TrackHeight, VolumeTrackLength);

            const int filled = static_cast<int>(VolumeTrackLength * qBound(0.0f, state.volume, 1.0f));
            layout.volumeFilled = QRect(trackLeft,
                                        layout.volumeSlider.bottom() - filled + 1,
                                        TrackHeight,
                                        filled);

            layout.volumeHandle = QRect(popupLeft + (ButtonSize - HandleRadius * 2) / 2,
                                        layout.volumeFilled.top() - HandleRadius,
                                        HandleRadius * 2,
                                        HandleRadius * 2);
        }
    }

    const int timeLeft = layout.play.right() + Gap;
    const int timeRight = trailingLeft - Gap;
    if (timeRight > timeLeft)
        layout.time = QRect(timeLeft, rowTop, timeRight - timeLeft, ButtonSize);

    if (state.durationMs > 0 && layout.seek.width() > 0) {
        const double fraction = qBound(0.0, static_cast<double>(state.positionMs) / state.durationMs, 1.0);
        const int filled = static_cast<int>(layout.seek.width() * fraction);
        layout.seekFilled = QRect(layout.seek.left(), layout.seek.top(), filled, TrackHeight);

        const double bufferedFraction = qBound(0.0, static_cast<double>(state.bufferedMs) / state.durationMs, 1.0);
        const int buffered = static_cast<int>(layout.seek.width() * bufferedFraction);
        if (buffered > filled)
            layout.seekBuffered = QRect(layout.seek.left(), layout.seek.top(), buffered, TrackHeight);

        const int centreX = layout.seek.left() + filled;
        const int centreY = layout.seek.center().y();
        layout.seekHandle = QRect(centreX - HandleRadius,
                                  centreY - HandleRadius,
                                  HandleRadius * 2,
                                  HandleRadius * 2);
    }

    return layout;
}

Hit hitTest(const Layout &layout, const QPoint &pos, const State &state)
{
    if (!layout.video.contains(pos))
        return Hit::None;

    if (state.volumeExpanded && !layout.volumeSlider.isNull()) {
        const QRect volumeGrab = layout.volumeSlider.adjusted(-SeekGrabSlack, -SeekGrabSlack,
                                                              SeekGrabSlack, SeekGrabSlack);
        if (volumeGrab.contains(pos))
            return Hit::VolumeSlider;

        if (!layout.volumePopup.isNull() && layout.volumePopup.contains(pos))
            return Hit::None;
    }

    if (!layout.bar.isNull() && layout.bar.contains(pos)) {
        const QRect seekGrab = layout.seek.adjusted(0, -SeekGrabSlack, 0, SeekGrabSlack);
        if (!layout.seek.isNull() && seekGrab.contains(pos))
            return Hit::Seek;

        if (!layout.play.isNull() && layout.play.contains(pos))
            return Hit::Play;
        if (!layout.volume.isNull() && layout.volume.contains(pos))
            return Hit::Volume;
        if (!layout.fullscreen.isNull() && layout.fullscreen.contains(pos))
            return Hit::Fullscreen;

        return Hit::None;
    }

    return Hit::Surface;
}

void paint(QPainter *painter, const Layout &layout, const State &state)
{
    if (layout.bar.isNull())
        return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient scrim(layout.bar.topLeft(), layout.bar.bottomLeft());
    scrim.setColorAt(0.0, QColor(0, 0, 0, 0));
    scrim.setColorAt(1.0, QColor(0, 0, 0, ScrimAlpha));
    painter->fillRect(layout.bar, scrim);

    if (!layout.seek.isNull()) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(trackBackground());
        painter->drawRoundedRect(layout.seek, TrackHeight / 2.0, TrackHeight / 2.0);

        if (!layout.seekBuffered.isNull() && layout.seekBuffered.width() > 0) {
            painter->setBrush(QColor(255, 255, 255, 55));
            painter->drawRoundedRect(layout.seekBuffered, TrackHeight / 2.0, TrackHeight / 2.0);
        }

        if (!layout.seekFilled.isNull() && layout.seekFilled.width() > 0) {
            painter->setBrush(accent());
            painter->drawRoundedRect(layout.seekFilled, TrackHeight / 2.0, TrackHeight / 2.0);
        }

        if (!layout.seekHandle.isNull() && state.durationMs > 0) {
            painter->setBrush(Qt::white);
            painter->drawEllipse(layout.seekHandle);
        }
    }

    if (!layout.play.isNull()) {
        drawGlyph(painter,
                  layout.play,
                  state.playing ? Core::Theme::Icons::Name::Pause
                                : Core::Theme::Icons::Name::Play);
    }

    if (!layout.time.isNull()) {
        painter->setPen(QColor(255, 255, 255, 220));
        const QString text = QStringLiteral("%1 / %2")
                                     .arg(formatTime(state.positionMs))
                                     .arg(formatTime(state.durationMs));
        const QString elided = painter->fontMetrics().elidedText(text, Qt::ElideRight, layout.time.width());
        painter->drawText(layout.time, Qt::AlignLeft | Qt::AlignVCenter, elided);
    }

    if (!layout.volumeSlider.isNull()) {
        painter->setPen(Qt::NoPen);

        if (!layout.volumePopup.isNull()) {
            painter->setBrush(QColor(0, 0, 0, ScrimAlpha));
            painter->drawRoundedRect(layout.volumePopup, ButtonSize / 2.0, ButtonSize / 2.0);
        }

        painter->setBrush(trackBackground());
        painter->drawRoundedRect(layout.volumeSlider, TrackHeight / 2.0, TrackHeight / 2.0);

        if (!layout.volumeFilled.isNull() && layout.volumeFilled.height() > 0) {
            painter->setBrush(accent());
            painter->drawRoundedRect(layout.volumeFilled, TrackHeight / 2.0, TrackHeight / 2.0);
        }

        if (!layout.volumeHandle.isNull()) {
            painter->setBrush(Qt::white);
            painter->drawEllipse(layout.volumeHandle);
        }
    }

    if (!layout.volume.isNull()) {
        const bool silent = state.muted || state.volume <= 0.0f;
        drawGlyph(painter,
                  layout.volume,
                  silent ? Core::Theme::Icons::Name::VolumeOff
                         : Core::Theme::Icons::Name::VolumeOn);
    }

    if (!layout.fullscreen.isNull()) {
        drawGlyph(painter,
                  layout.fullscreen,
                  state.fullscreen ? Core::Theme::Icons::Name::Minimize
                                   : Core::Theme::Icons::Name::Maximize);
    }

    painter->restore();
}

void paintPlayBadge(QPainter *painter, const QRect &videoRect)
{
    if (videoRect.isEmpty())
        return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const int size = std::min(48, std::min(videoRect.width(), videoRect.height()) / 2);
    if (size <= 0) {
        painter->restore();
        return;
    }

    const QRect badge(videoRect.center().x() - size / 2,
                      videoRect.center().y() - size / 2,
                      size,
                      size);

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, BadgeAlpha));
    painter->drawEllipse(badge);

    const int inset = size / 4;
    QPolygon triangle;
    triangle << QPoint(badge.left() + inset + 4, badge.top() + inset)
             << QPoint(badge.left() + inset + 4, badge.bottom() - inset)
             << QPoint(badge.right() - inset, badge.center().y());

    painter->setBrush(Qt::white);
    painter->drawPolygon(triangle);

    painter->restore();
}

QRect volumeHoverZone(const Layout &layout)
{
    if (layout.volume.isNull())
        return QRect();

    QRect zone = layout.volume;
    if (!layout.volumePopup.isNull())
        zone = zone.united(layout.volumePopup);

    return zone;
}

QRect fitRect(const QSize &content, const QRect &bounds)
{
    if (content.isEmpty() || bounds.isEmpty())
        return bounds;

    QSize scaled = content;
    scaled.scale(bounds.size(), Qt::KeepAspectRatio);

    return QRect(bounds.left() + (bounds.width() - scaled.width()) / 2,
                 bounds.top() + (bounds.height() - scaled.height()) / 2,
                 scaled.width(),
                 scaled.height());
}

QString formatTime(qint64 ms)
{
    if (ms < 0)
        ms = 0;

    const qint64 totalSeconds = ms / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;

    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
                .arg(hours)
                .arg(minutes, 2, 10, QLatin1Char('0'))
                .arg(seconds, 2, 10, QLatin1Char('0'));
    }

    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

qint64 positionForSeekX(const Layout &layout, int x, qint64 durationMs)
{
    if (layout.seek.width() <= 0 || durationMs <= 0)
        return 0;

    const double fraction = qBound(0.0, static_cast<double>(x - layout.seek.left()) / layout.seek.width(), 1.0);
    return static_cast<qint64>(fraction * durationMs);
}

float volumeForSliderY(const Layout &layout, int y)
{
    if (layout.volumeSlider.height() <= 0)
        return 0.0f;

    const double fraction = qBound(0.0,
                                   static_cast<double>(layout.volumeSlider.bottom() - y) / layout.volumeSlider.height(),
                                   1.0);
    return static_cast<float>(fraction);
}

} // namespace VideoControls
} // namespace UI
} // namespace Acheron
