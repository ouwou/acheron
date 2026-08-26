#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <optional>

#include "Core/Emoji/EmojiMatch.hpp"
#include "Core/Emoji/FrecencyTracker.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"

namespace Acheron {

namespace Discord {
class Client;
}

namespace Proto {
struct FrecencyUserSettings;
}

namespace Core {

struct UnicodeEmoji;
class ClientInstance;

class EmojiManager : public QObject
{
    Q_OBJECT
public:
    EmojiManager(Discord::Client *client, ClientInstance *instance, QObject *parent = nullptr);

    void setGuildEmojis(Snowflake guildId, const QList<Discord::Emoji> &emojis);
    void removeGuild(Snowflake guildId);

    [[nodiscard]] QList<EmojiMatch> search(const QString &query, Snowflake channelId, int maxResults = 10);

    void trackMessageEmojis(const QString &content);
    void trackReaction(const QString &reactionEmoji);

    // write back to settings-proto/2
    void savePendingUsage();

public slots:
    void onReady();
    void onGuildEmojisUpdated(const Discord::GuildEmojisUpdate &event);
    void onUserSettingsProtoUpdated(const Discord::UserSettingsProtoUpdate &event);

private:
    struct CustomEmoji
    {
        Discord::Emoji emoji;
        Snowflake guildId;
        QString originalName;
        // `name~1`, `name~2`
        QString disambiguatedName;
        QString normalizedName;
    };

    struct UnicodeCandidate
    {
        const UnicodeEmoji *emoji = nullptr;
        QStringList normalizedNames;
    };

    struct PendingUsage
    {
        QString key;
        uint64_t timestamp = 0;
        bool reaction = false;
    };

    // for querying emoji usability
    struct UsabilityContext
    {
        Snowflake channelId;
        Snowflake channelGuildId;
        Snowflake selfId;
        bool premium = false;
        QHash<Snowflake, QSet<Snowflake>> rolesByGuild;
    };

    void fetchSettings();
    void adoptSettings(const Proto::FrecencyUserSettings &settings, bool partial);
    void replayPendingUsages();
    void seedDefaultsIfEmpty();

    void trackUsage(const QString &key, bool reaction);
    void trackSurrogates(const QString &content);

    void onSyncTimer();

    const QList<CustomEmoji> &customEmojis();
    const QList<UnicodeCandidate> &unicodeCandidates();
    bool isUsable(const CustomEmoji &custom, UsabilityContext &context) const;

    [[nodiscard]] static uint64_t nowMs();
    [[nodiscard]] static QString unicodeKey(const QString &surrogates);

    Discord::Client *client;
    ClientInstance *instance;

    QHash<Snowflake /*guildId*/, QList<Discord::Emoji>> guildEmojis;
    QList<CustomEmoji> customCandidates;
    bool customCandidatesDirty = true;
    QList<UnicodeCandidate> unicodeCandidateCache;

    FrecencyTracker chatTracker;
    FrecencyTracker reactionTracker;

    QList<PendingUsage> pendingUsages;
    std::optional<uint32_t> dataVersion;
    bool loaded = false;
    bool saveInFlight = false;

    QTimer syncTimer;
};

} // namespace Core
} // namespace Acheron
