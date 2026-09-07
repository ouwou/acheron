#include "MessageSegments.hpp"

#include <algorithm>
#include <set>

namespace Acheron {
namespace Core {

MessageSegments::Location MessageSegments::locate(Snowflake id) const
{
    if (!id.isValid())
        return {};

    for (int r = 0; r < static_cast<int>(runs.size()); ++r) {
        const auto &ids = runs[r].ids;
        if (id < ids.front() || id > ids.back())
            continue;
        auto it = std::lower_bound(ids.begin(), ids.end(), id);
        if (it != ids.end() && *it == id)
            return { r, static_cast<int>(std::distance(ids.begin(), it)) };
        return {};
    }
    return {};
}

MessageSegments::Position MessageSegments::find(Snowflake id) const
{
    Location loc = locate(id);
    if (loc.run < 0)
        return {};
    return { &runs[loc.run], loc.index };
}

const MessageSegments::Run *MessageSegments::tail() const
{
    for (const auto &run : runs) {
        if (run.isTail)
            return &run;
    }
    return nullptr;
}

void MessageSegments::merge(const QList<Snowflake> &sortedIds, Snowflake anchorId, bool reachesPresent)
{
    std::set<int> joined;
    if (Location anchor = locate(anchorId); anchor.run >= 0)
        joined.insert(anchor.run);
    for (const auto &id : sortedIds) {
        if (Location loc = locate(id); loc.run >= 0)
            joined.insert(loc.run);
    }
    if (reachesPresent) {
        for (int r = 0; r < static_cast<int>(runs.size()); ++r) {
            if (runs[r].isTail)
                joined.insert(r);
        }
    }

    if (joined.empty() && sortedIds.isEmpty())
        return;

    Run merged;
    merged.isTail = reachesPresent;
    std::vector<Snowflake> all(sortedIds.begin(), sortedIds.end());
    for (int r : joined) {
        merged.isTail = merged.isTail || runs[r].isTail;
        all.insert(all.end(), runs[r].ids.begin(), runs[r].ids.end());
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    merged.ids.assign(all.begin(), all.end());

    for (auto it = joined.rbegin(); it != joined.rend(); ++it)
        runs.erase(runs.begin() + *it);

    if (merged.ids.empty())
        return;

    auto insertAt = std::lower_bound(runs.begin(),
                                     runs.end(),
                                     merged.ids.front(),
                                     [](const Run &run, Snowflake id) { return run.ids.front() < id; });
    runs.insert(insertAt, std::move(merged));
}

void MessageSegments::setTail(const QList<Snowflake> &sortedIds)
{
    for (auto &run : runs)
        run.isTail = false;
    merge(sortedIds, Snowflake::Invalid, true);
}

void MessageSegments::remove(Snowflake id)
{
    Location loc = locate(id);
    if (loc.run < 0)
        return;

    auto &run = runs[loc.run];
    run.ids.erase(run.ids.begin() + loc.index);
    if (run.ids.empty())
        runs.erase(runs.begin() + loc.run);
}

} // namespace Core
} // namespace Acheron
