#pragma once

#include <QPoint>
#include <QRect>
#include <QString>

class QPainter;

namespace Acheron {

namespace Core {
namespace Media {
class Player;
} // namespace Media
} // namespace Core

namespace UI {

namespace VideoControls {

struct State
{
    bool playing = false;
    bool muted = false;
    float volume = 1.0f;
    qint64 positionMs = 0;
    qint64 durationMs = 0;
    qint64 bufferedMs = 0;
    bool volumeExpanded = false;
    bool fullscreen = false;
    bool audioOnly = false;
    bool voiceMessage = false;
};

struct Layout
{
    QRect video;
    QRect bar;
    QRect seek;
    QRect seekBuffered;
    QRect seekFilled;
    QRect seekHandle;
    QRect play;
    QRect time;
    QRect volume;
    QRect volumePopup;
    QRect volumeSlider;
    QRect volumeFilled;
    QRect volumeHandle;
    QRect fullscreen;
    QRect voiceBadge;
};

enum class Hit {
    None,
    Surface,
    Play,
    Seek,
    Volume,
    VolumeSlider,
    Fullscreen,
};

State stateFor(const Core::Media::Player *player, bool volumeExpanded, bool fullscreen = false);

struct Drag
{
    enum class Kind {
        None,
        Seek,
        Volume,
    };

    Kind kind = Kind::None;
    QString key;

    [[nodiscard]] bool active() const { return kind != Kind::None; }
    [[nodiscard]] bool isSeek() const { return kind == Kind::Seek; }

    void end()
    {
        kind = Kind::None;
        key.clear();
    }
};

enum class ReleaseResult {
    None,
    Handled,
    ToggleFullscreen,
};

bool beginDrag(Core::Media::Player *player, const Layout &layout, const State &state, const QPoint &pos, Drag &drag, const QString &key = QString());
void applyDrag(Core::Media::Player *player, const Layout &layout, const QPoint &pos, const Drag &drag);
ReleaseResult handleRelease(Core::Media::Player *player, const Layout &layout, const State &state, const QPoint &pos);

int barHeight();

Layout calculate(const QRect &videoRect, const State &state);

Hit hitTest(const Layout &layout, const QPoint &pos, const State &state);

void paint(QPainter *painter, const Layout &layout, const State &state);

void paintAudioBase(QPainter *painter, const QRect &barRect);

void paintPlayBadge(QPainter *painter, const QRect &videoRect);

QRect volumeHoverZone(const Layout &layout);

QRect fitRect(const QSize &content, const QRect &bounds);

QString formatTime(qint64 ms);

qint64 positionForSeekX(const Layout &layout, int x, qint64 durationMs);

bool sliderIsHorizontal(const Layout &layout);

float volumeForSliderPos(const Layout &layout, const QPoint &pos);

} // namespace VideoControls
} // namespace UI
} // namespace Acheron
