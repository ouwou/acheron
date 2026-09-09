#pragma once

#include <QList>
#include <QString>

#include <optional>

#include "Discord/Entities.hpp"

namespace Acheron {
namespace Core {
namespace ActivityFormat {

// sorted by rank, then rich, then most recent
QList<Discord::Activity> sortAndFilter(const QList<Discord::Activity> &activities);

const Discord::Activity *primary(const QList<Discord::Activity> &sorted);
const Discord::Activity *custom(const QList<Discord::Activity> &activities);

// bare status text
QString secondaryText(const Discord::Activity &activity);

QString cardHeading(const Discord::Activity &activity);

bool headingHasName(const Discord::Activity &activity);
QString partyText(const Discord::Activity &activity);

struct TimerInfo
{
    enum class Kind {
        None,
        Elapsed,
        Countdown,
        Progress,
    };

    Kind kind = Kind::None;
    qint64 startMs = 0;
    qint64 endMs = 0;
};

TimerInfo timerFor(const Discord::Activity &activity);

QString timerText(const TimerInfo &info, qint64 nowMs);
double progress(const TimerInfo &info, qint64 nowMs);

} // namespace ActivityFormat
} // namespace Core
} // namespace Acheron
