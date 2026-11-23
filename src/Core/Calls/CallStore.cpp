#include "CallStore.hpp"

namespace Acheron {
namespace Core {

CallStore::CallStore(QObject *parent) : QObject(parent) { }

bool CallStore::claimConnect(Snowflake channelId, CallConnect rule)
{
    auto it = calls.find(channelId);
    if (it != calls.end())
        return rule == CallConnect::Always;

    CallRecord placeholder;
    placeholder.channelId = channelId;
    calls.insert(channelId, placeholder);
    return true;
}

void CallStore::handleCallCreate(const Discord::CallCreate &event)
{
    CallRecord record;
    record.channelId = event.channelId;
    if (event.messageId.hasValue())
        record.messageId = event.messageId.get();
    record.region = event.region;
    record.ongoingRings = event.ongoingRings;

    calls.insert(record.channelId, record);
    emit callChanged(record.channelId);
}

void CallStore::handleCallUpdate(const Discord::CallUpdate &event)
{
    CallRecord &record = calls[event.channelId.get()];
    record.channelId = event.channelId;
    record.messageId = event.messageId.hasValue() ? event.messageId.get() : Snowflake::Invalid;
    record.region = event.region;
    record.ongoingRings = event.ongoingRings;

    emit callChanged(record.channelId);
}

void CallStore::handleCallDelete(const Discord::CallDelete &event)
{
    const Snowflake channelId = event.channelId;
    const bool unavailable = event.unavailable.valueOr(false);

    auto it = calls.find(channelId);
    if (unavailable && it != calls.end()) {
        it->unavailable = true;
    } else {
        // the channel stays known so opening the DM doesnt ask again
        CallRecord ended;
        ended.channelId = channelId;
        ended.unavailable = unavailable;
        calls.insert(channelId, ended);
    }

    emit callChanged(channelId);
}

void CallStore::forget(Snowflake channelId)
{
    if (calls.remove(channelId))
        emit callChanged(channelId);
}

void CallStore::reset()
{
    const QList<Snowflake> channelIds = calls.keys();
    calls.clear();
    for (Snowflake channelId : channelIds)
        emit callChanged(channelId);
}

std::optional<CallRecord> CallStore::call(Snowflake channelId) const
{
    auto it = calls.constFind(channelId);
    if (it == calls.constEnd())
        return std::nullopt;
    return it.value();
}

bool CallStore::isActive(Snowflake channelId) const
{
    auto it = calls.constFind(channelId);
    return it != calls.constEnd() && it->isActive();
}

bool CallStore::isRinging(Snowflake channelId, Snowflake userId) const
{
    auto it = calls.constFind(channelId);
    return it != calls.constEnd() && it->isActive() && it->ongoingRings.contains(userId);
}

} // namespace Core
} // namespace Acheron
