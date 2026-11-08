#pragma once

#include <QHash>
#include <QList>
#include <QObject>

#include <functional>
#include <optional>

#include "Discord/Entities.hpp"
#include "Result.hpp"
#include "Snowflake.hpp"

namespace Acheron {

namespace Discord {
class Client;
struct ChannelDelete;
struct ThreadDelete;
struct GuildDelete;
} // namespace Discord

namespace Core {

class ChannelPins : public QObject
{
    Q_OBJECT
public:
    static constexpr int PageSize = 25;
    static constexpr int MaxPerChannel = 250;

    enum class LoadState {
        Loading,
        HasMore,
        Finished,
        Failed,
    };

    struct Pinned
    {
        QDateTime pinnedAt;
        Discord::Message message;
    };

    struct Channel
    {
        Snowflake guildId;
        LoadState state = LoadState::Loading;
        QList<Pinned> newestFirst;
    };

    using PrepareMessage = std::function<void(Discord::Message &)>;
    ChannelPins(Discord::Client *client, PrepareMessage prepare, QObject *parent = nullptr);

    [[nodiscard]] const Channel *channel(Snowflake channelId) const;

    void fetchFirstPage(Snowflake channelId);
    void fetchPageAfterOldest(Snowflake channelId);

    void pin(Snowflake channelId, Snowflake messageId);
    void unpin(Snowflake channelId, Snowflake messageId);

    void applyMessageUpdate(const Discord::Message &update);
    void forgetMessage(Snowflake channelId, Snowflake messageId);

signals:
    void pinsChanged(Core::Snowflake channelId);
    void loadStateChanged(Core::Snowflake channelId);
    void pinFailed(Core::Snowflake channelId, int errorCode, const QString &errorMessage);
    void unpinFailed(Core::Snowflake channelId, Core::Snowflake messageId);

private slots:
    void onReady();
    void onChannelDeleted(const Discord::ChannelDelete &event);
    void onThreadDeleted(const Discord::ThreadDelete &event);
    void onGuildDeleted(const Discord::GuildDelete &event);

private:
    void fetch(Snowflake channelId, std::optional<QDateTime> pinnedBefore);
    void forgetChannel(Snowflake channelId);

    Discord::Client *client;
    PrepareMessage prepare;
    QHash<Snowflake, Channel> channels;
};

} // namespace Core
} // namespace Acheron
