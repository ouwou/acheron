#include "FrecencyTracker.hpp"

#include <algorithm>
#include <cmath>

#include <QDateTime>

namespace Acheron {
namespace Core {

namespace {

constexpr double MS_PER_DAY = 86400000.0;

uint64_t lastUse(const FrecencyTracker::Entry &entry)
{
    return entry.recentUses.isEmpty() ? 0 : entry.recentUses.last();
}

int32_t computeFrecency(FrecencyTracker::Formula formula, const FrecencyTracker::Entry &entry,
                        uint32_t maxTotalUse)
{
    switch (formula) {
    case FrecencyTracker::Formula::Chat:
        return static_cast<int32_t>(
                std::ceil(entry.totalUses * (entry.score / entry.recentUses.size())));
    case FrecencyTracker::Formula::Reaction: {
        if (maxTotalUse == 0)
            return 0;
        const double ratio = static_cast<double>(entry.totalUses) / maxTotalUse;
        return static_cast<int32_t>(std::trunc(1000.0 * (ratio * 0.2 + (entry.score / 1000.0) * 0.8)));
    }
    }
    return 0;
}

} // namespace

int FrecencyTracker::defaultComputeWeight(int ageInDays)
{
    if (ageInDays <= 3)
        return 100;
    if (ageInDays <= 15)
        return 70;
    if (ageInDays <= 30)
        return 50;
    if (ageInDays <= 45)
        return 30;
    if (ageInDays <= 80)
        return 10;
    return 1;
}

FrecencyTracker::FrecencyTracker(Options options)
    : options(options)
{
}

FrecencyTracker::Options FrecencyTracker::chatEmojiOptions()
{
    return { 42, 10, Formula::Chat };
}

FrecencyTracker::Options FrecencyTracker::reactionEmojiOptions()
{
    return { 42, 10, Formula::Reaction };
}

int FrecencyTracker::ageInDays(uint64_t nowMs, uint64_t tMs)
{
    const double diff = (static_cast<double>(nowMs) - static_cast<double>(tMs)) / MS_PER_DAY;
    return static_cast<int>(std::trunc(diff));
}

void FrecencyTracker::markDirty()
{
    dirty = true;
}

void FrecencyTracker::track(const QString &key, std::optional<uint64_t> timestampMs,
                            uint32_t usesSinceLastTrack)
{
    if (key.isNull())
        return;

    const uint64_t timestamp =
            timestampMs.value_or(static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()));

    auto it = history.find(key);
    if (it == history.end()) {
        Entry entry;
        entry.totalUses = usesSinceLastTrack;
        entry.recentUses.append(timestamp);
        history.insert(key, entry);
    } else {
        Entry &entry = *it;
        entry.frecency = -1;
        entry.totalUses += usesSinceLastTrack;
        entry.recentUses.append(timestamp);
        if (timestampMs)
            std::sort(entry.recentUses.begin(), entry.recentUses.end());
        while (entry.recentUses.size() > options.maxSamples)
            entry.recentUses.removeFirst();
    }

    markDirty();
}

void FrecencyTracker::overwriteHistory(const QHash<QString, Entry> &newHistory)
{
    history = newHistory;
    for (auto it = history.begin(); it != history.end(); ++it) {
        it->recentUses.erase(std::remove(it->recentUses.begin(), it->recentUses.end(), 0),
                             it->recentUses.end());
        std::sort(it->recentUses.begin(), it->recentUses.end());
        it->frecency = -1;
    }
    markDirty();
}

void FrecencyTracker::compute(uint64_t nowMs)
{
    uint32_t maxTotalUse = 0;
    for (auto it = history.constBegin(); it != history.constEnd(); ++it)
        maxTotalUse = std::max(maxTotalUse, it->totalUses);

    for (auto it = history.begin(); it != history.end();) {
        Entry &entry = *it;
        if (entry.frecency != -1) {
            ++it;
            continue;
        }

        entry.score = 0;
        const int samples = std::min<int>(entry.recentUses.size(), options.maxSamples);
        for (int i = 0; i < samples; ++i)
            entry.score += defaultComputeWeight(ageInDays(nowMs, entry.recentUses.at(i)));

        // Every weight is positive, so a zero score means the entry has no samples left at all.
        if (entry.score <= 0) {
            it = history.erase(it);
            continue;
        }

        entry.frecency = computeFrecency(options.formula, entry, maxTotalUse);
        ++it;
    }

    QList<std::pair<QString, int32_t>> ranked;
    ranked.reserve(history.size());
    for (auto it = history.constBegin(); it != history.constEnd(); ++it)
        ranked.append({ it.key(), it->frecency });

    // Discord sorts by -frecency only; the key tiebreak keeps our order deterministic since QHash
    // iteration is not.
    std::sort(ranked.begin(), ranked.end(),
              [](const std::pair<QString, int32_t> &a, const std::pair<QString, int32_t> &b) {
                  if (a.second != b.second)
                      return a.second > b.second;
                  return a.first < b.first;
              });

    const int count = std::min<int>(ranked.size(), std::max(options.numFrequentlyItems, 0));
    frequentlyKeys.clear();
    frequentlyKeys.reserve(count);
    for (int i = 0; i < count; ++i)
        frequentlyKeys.append(ranked.at(i).first);

    dirty = false;
}

void FrecencyTracker::refresh(uint64_t nowMs)
{
    if (dirty)
        compute(nowMs);
}

const QStringList &FrecencyTracker::frequently(uint64_t nowMs)
{
    refresh(nowMs);
    return frequentlyKeys;
}

std::optional<double> FrecencyTracker::score(const QString &key) const
{
    const auto it = history.constFind(key);
    if (it == history.constEnd())
        return std::nullopt;
    return it->score;
}

std::optional<int32_t> FrecencyTracker::frecency(const QString &key) const
{
    const auto it = history.constFind(key);
    if (it == history.constEnd())
        return std::nullopt;
    return it->frecency;
}

void FrecencyTracker::reset()
{
    history.clear();
    frequentlyKeys.clear();
    dirty = false;
}

QHash<QString, FrecencyTracker::Entry> FrecencyTracker::capForPersistence(const QHash<QString, Entry> &source, int limit)
{
    const int keep = std::max(limit, 0);

    QList<std::pair<QString, Entry>> entries;
    entries.reserve(source.size());
    for (auto it = source.constBegin(); it != source.constEnd(); ++it)
        entries.append({ it.key(), it.value() });

    if (entries.size() > keep) {
        std::sort(entries.begin(), entries.end(),
                  [](const std::pair<QString, Entry> &a, const std::pair<QString, Entry> &b) {
                      const uint64_t lhs = lastUse(a.second);
                      const uint64_t rhs = lastUse(b.second);
                      if (lhs != rhs)
                          return lhs > rhs;
                      return a.first < b.first;
                  });
        entries.erase(entries.begin() + keep, entries.end());
    }

    QHash<QString, Entry> capped;
    capped.reserve(entries.size());
    for (const auto &pair : entries) {
        Entry entry = pair.second;
        entry.score = std::round(entry.score);
        capped.insert(pair.first, entry);
    }

    return capped;
}

} // namespace Core
} // namespace Acheron
