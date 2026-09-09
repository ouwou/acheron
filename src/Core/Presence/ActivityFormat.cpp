#include "ActivityFormat.hpp"

#include <QCoreApplication>

#include <algorithm>

namespace Acheron {
namespace Core {
namespace ActivityFormat {

using Discord::Activity;
using Discord::ActivityType;

namespace {

int sortRank(const Activity &activity)
{
    switch (activity.kind()) {
    case ActivityType::CUSTOM:
        return 4;
    case ActivityType::COMPETING:
        return 3;
    case ActivityType::STREAMING:
        return 2;
    case ActivityType::PLAYING:
        return 1;
    default:
        return 0;
    }
}

bool isRich(const Activity &activity)
{
    if (activity.isCustom())
        return false;

    if (!activity.detailsText().isEmpty() || !activity.stateText().isEmpty())
        return true;
    if (activity.party.hasValue() && activity.party->hasSize())
        return true;
    if (!activity.assets.hasValue())
        return false;

    // the client does this apparently
    return activity.assets->largeImage.hasValue() || activity.assets->smallText.hasValue();
}

} // namespace

QList<Activity> sortAndFilter(const QList<Activity> &activities)
{
    QList<Activity> sorted = activities;

    std::stable_sort(sorted.begin(), sorted.end(), [](const Activity &a, const Activity &b) {
        const int rankA = sortRank(a);
        const int rankB = sortRank(b);
        if (rankA != rankB)
            return rankA > rankB;

        const bool richA = isRich(a);
        const bool richB = isRich(b);
        if (richA != richB)
            return richA;

        return a.createdAtMs() > b.createdAtMs();
    });

    QList<Activity> filtered;
    filtered.reserve(sorted.size());
    bool seenPlaying = false;
    for (const Activity &activity : sorted) {
        if (activity.kind() == ActivityType::PLAYING) {
            if (seenPlaying)
                continue;
            seenPlaying = true;
        }
        filtered.append(activity);
    }

    return filtered;
}

const Activity *primary(const QList<Activity> &sorted)
{
    for (const Activity &activity : sorted) {
        if (activity.kind() == ActivityType::HANG)
            continue;
        return &activity;
    }
    return nullptr;
}

const Activity *custom(const QList<Activity> &activities)
{
    for (const Activity &activity : activities) {
        if (activity.isCustom())
            return &activity;
    }
    return nullptr;
}

QString secondaryText(const Activity &activity)
{
    if (activity.isCustom())
        return activity.stateText();

    const QString name = activity.nameText();
    const QString details = activity.detailsText();
    const QString state = activity.stateText();

    QString text = activity.kind() == ActivityType::STREAMING && !details.isEmpty() ? details : name;

    if (activity.statusDisplayType.hasValue()) {
        switch (activity.statusDisplayType.get()) {
        case Discord::StatusDisplayType::NAME:
            if (!name.isEmpty())
                text = name;
            break;
        case Discord::StatusDisplayType::STATE:
            if (!state.isEmpty())
                text = state;
            break;
        case Discord::StatusDisplayType::DETAILS:
            if (!details.isEmpty())
                text = details;
            break;
        }
    }

    return text;
}

QString cardHeading(const Activity &activity)
{
    const QString name = activity.nameText();

    switch (activity.kind()) {
    case ActivityType::STREAMING:
        return QCoreApplication::translate("ActivityFormat", "Streaming");
    case ActivityType::LISTENING:
        return QCoreApplication::translate("ActivityFormat", "Listening to %1").arg(name);
    case ActivityType::WATCHING:
        return QCoreApplication::translate("ActivityFormat", "Watching %1").arg(name);
    case ActivityType::COMPETING:
        return QCoreApplication::translate("ActivityFormat", "Competing in %1").arg(name);
    default:
        return QCoreApplication::translate("ActivityFormat", "Playing a game");
    }
}

bool headingHasName(const Activity &activity)
{
    switch (activity.kind()) {
    case ActivityType::LISTENING:
    case ActivityType::WATCHING:
    case ActivityType::COMPETING:
        return true;
    default:
        return false;
    }
}

QString partyText(const Activity &activity)
{
    const QString state = activity.stateText();
    if (!activity.party.hasValue() || !activity.party->hasSize())
        return state;

    const Discord::ActivityParty &party = activity.party.get();
    const QString count =
            party.maxSize > 0
                    ? QCoreApplication::translate("ActivityFormat", "%1 of %2", "party size")
                              .arg(party.currentSize)
                              .arg(party.maxSize)
                    : QString::number(party.currentSize);

    if (state.isEmpty())
        return QStringLiteral("(%1)").arg(count);
    return QStringLiteral("%1 (%2)").arg(state, count);
}

TimerInfo timerFor(const Activity &activity)
{
    TimerInfo info;

    if (activity.isCustom() || activity.kind() == ActivityType::HANG)
        return info;

    const qint64 start = activity.startMs();
    if (start <= 0)
        return info;

    info.startMs = start;
    info.endMs = activity.endMs();

    if (info.endMs <= 0)
        info.kind = TimerInfo::Kind::Elapsed;
    else if (activity.kind() == ActivityType::LISTENING)
        info.kind = TimerInfo::Kind::Progress;
    else
        info.kind = TimerInfo::Kind::Countdown;

    return info;
}

namespace {

QString formatDuration(qint64 ms)
{
    if (ms < 0)
        ms = 0;

    const qint64 total = ms / 1000;
    const qint64 hours = total / 3600;
    const qint64 minutes = (total % 3600) / 60;
    const qint64 seconds = total % 60;

    if (hours > 0)
        return QStringLiteral("%1:%2:%3")
                .arg(hours)
                .arg(minutes, 2, 10, QLatin1Char('0'))
                .arg(seconds, 2, 10, QLatin1Char('0'));

    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

} // namespace

QString timerText(const TimerInfo &info, qint64 nowMs)
{
    switch (info.kind) {
    case TimerInfo::Kind::Elapsed:
        return QCoreApplication::translate("ActivityFormat", "%1 elapsed")
                .arg(formatDuration(nowMs - info.startMs));
    case TimerInfo::Kind::Countdown:
        if (nowMs < info.endMs)
            return QCoreApplication::translate("ActivityFormat", "%1 left")
                    .arg(formatDuration(info.endMs - nowMs));
        return QCoreApplication::translate("ActivityFormat", "%1 elapsed")
                .arg(formatDuration(nowMs - info.startMs));
    case TimerInfo::Kind::Progress: {
        const qint64 elapsed = qMin(info.endMs, nowMs) - info.startMs;
        return QCoreApplication::translate("ActivityFormat", "%1 of %2", "playback position")
                .arg(formatDuration(elapsed), formatDuration(info.endMs - info.startMs));
    }
    case TimerInfo::Kind::None:
        break;
    }

    return {};
}

double progress(const TimerInfo &info, qint64 nowMs)
{
    const qint64 span = info.endMs - info.startMs;
    if (span <= 0)
        return 0.0;

    const qint64 elapsed = qBound<qint64>(0, nowMs - info.startMs, span);
    return static_cast<double>(elapsed) / static_cast<double>(span);
}

} // namespace ActivityFormat
} // namespace Core
} // namespace Acheron
