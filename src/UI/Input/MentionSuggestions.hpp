#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include "Core/Emoji/EmojiMatch.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

class QTimer;

namespace Acheron {
namespace Core {
class ClientInstance;
}
namespace UI {

// Asks the gateway for guild members matching what is typed after @, like the official client: each guild and query
// once a minute, sent 200 ms after the last keystroke.
class MemberSearch : public QObject
{
    Q_OBJECT
public:
    explicit MemberSearch(QObject *parent = nullptr);

    void setInstance(Core::ClientInstance *instance);
    void request(Core::Snowflake guildId, const QString &query);

private:
    void send();

    QPointer<Core::ClientInstance> instance;
    QTimer *timer;
    QHash<QString, QDateTime> recentQueries;
    Core::Snowflake pendingGuildId;
    QString pendingQuery;
};

namespace MentionSuggestions {

// users, then @everyone / @here, then roles, ten at most in all
QList<Core::EmojiMatch> forMention(Core::ClientInstance *instance, Core::Snowflake channelId, const QString &query, const QVector<Discord::Message> &loadedMessages, MemberSearch &memberSearch);
// a query starting with ! lists voice channels instead of text channels
QList<Core::EmojiMatch> forChannel(Core::ClientInstance *instance, Core::Snowflake channelId, const QString &query);

} // namespace MentionSuggestions

} // namespace UI
} // namespace Acheron
