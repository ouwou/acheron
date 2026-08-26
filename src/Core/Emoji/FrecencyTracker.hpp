#pragma once

#include <cstdint>
#include <optional>

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace Acheron {
namespace Core {

// port from discord
class FrecencyTracker
{
public:
    // <=3 -> 100, <=15 -> 70, <=30 -> 50, <=45 -> 30, <=80 -> 10, else 1
    static int defaultComputeWeight(int ageDays);

    struct Entry
    {
        uint32_t totalUses = 0;
        QList<uint64_t> recentUses; // ms since epoch, ascending
        int32_t frecency = -1; // -1 means needs recompute
        double score = 0;
    };

    enum class Formula {
        // ceil(totalUses * score / numOfRecentUses)
        Chat,
        // trunc(1000 * (0.2 * totalUses / maxTotalUses + 0.8 * score / 1000))
        Reaction,
    };

    struct Options
    {
        int numFrequentlyItems = 32;
        int maxSamples = 10;
        Formula formula = Formula::Chat;
    };

    explicit FrecencyTracker(Options options = {});

    static Options chatEmojiOptions();
    static Options reactionEmojiOptions();

    void track(const QString &key, std::optional<uint64_t> timestampMs = {}, uint32_t usesSinceLastTrack = 1);
    // replace history from server state
    void overwriteHistory(const QHash<QString, Entry> &history);
    void compute(uint64_t nowMs);
    // lazy. nowMs is param for tests
    void refresh(uint64_t nowMs);

    const QStringList &frequently(uint64_t nowMs);

    std::optional<double> score(const QString &key) const;
    std::optional<int32_t> frecency(const QString &key) const;

    const QHash<QString, Entry> &usageHistory() const { return history; }
    bool isDirty() const { return dirty; }
    void reset();

    static int ageInDays(uint64_t nowMs, uint64_t tMs);

    // keep only a limited amount to match client
    static QHash<QString, Entry> capForPersistence(const QHash<QString, Entry> &source, int limit);

private:
    void markDirty();

    Options options;
    QHash<QString, Entry> history;
    QStringList frequentlyKeys;
    bool dirty = false;
};

} // namespace Core
} // namespace Acheron
