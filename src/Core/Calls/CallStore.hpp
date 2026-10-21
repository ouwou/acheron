#pragma once

#include <QHash>
#include <QList>
#include <QObject>

#include <optional>

#include "Core/Snowflake.hpp"
#include "Discord/Events.hpp"

namespace Acheron {
namespace Core {

struct CallRecord
{
    Snowflake channelId;
    Snowflake messageId;
    std::optional<QString> region;
    QHash<Snowflake, Snowflake> ongoingRings;
    bool unavailable = false;

    [[nodiscard]] bool isActive() const { return !unavailable && region.has_value(); }
};

// kept as the official client's CallStore keeps them. calls are never in READY: they arrive as
// CALL_CREATE, on connect and in answer to CALL_CONNECT (opcode 13)
enum class CallConnect {
    OncePerChannel,
    // a fresh or resumed session asks again
    Always,
};

class CallStore : public QObject
{
    Q_OBJECT
public:
    explicit CallStore(QObject *parent = nullptr);

    // remembers each channel it says yes to
    [[nodiscard]] bool claimConnect(Snowflake channelId, CallConnect rule);

    void handleCallCreate(const Discord::CallCreate &event);
    void handleCallUpdate(const Discord::CallUpdate &event);
    void handleCallDelete(const Discord::CallDelete &event);
    void forget(Snowflake channelId);
    void reset();

    [[nodiscard]] std::optional<CallRecord> call(Snowflake channelId) const;
    [[nodiscard]] bool isActive(Snowflake channelId) const;
    [[nodiscard]] bool isRinging(Snowflake channelId, Snowflake userId) const;

signals:
    void callChanged(Acheron::Core::Snowflake channelId);

private:
    QHash<Snowflake, CallRecord> calls;
};

} // namespace Core
} // namespace Acheron
