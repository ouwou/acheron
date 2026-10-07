#include "Core/Emoji/EmojiManager.hpp"

#include <algorithm>

#include <QByteArray>
#include <QDateTime>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>

#include "Core/ClientInstance.hpp"
#include "Core/Emoji/UnicodeEmojiIndex.hpp"
#include "Core/EmojiSegmenter.hpp"
#include "Core/Logging.hpp"
#include "Core/PermissionManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"
#include "Discord/Enums.hpp"
#include "Proto/FrecencySettings.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/UserSettings.hpp"

namespace Acheron {
namespace Core {

namespace {

constexpr int PERSISTED_ENTRY_LIMIT = 100;
constexpr int FIRST_SAVE_MS = 10 * 1000;
constexpr int FIRST_SAVE_JITTER_MS = 10 * 1000;
constexpr int PERIODIC_SAVE_MS = 2 * 60 * 60 * 1000;
constexpr int PERIODIC_SAVE_JITTER_MS = 10 * 60 * 1000;
constexpr int RATE_LIMIT_BACKOFF_MS = 30 * 1000;
constexpr int FETCH_RETRY_MS = 60 * 1000;
constexpr int MAX_PENDING_USAGES = 1000;

// straight from discord
const char *const DEFAULT_CHAT_EMOJIS[] = { "thumbsup", "eyes", "laughing", "watermelon",
                                            "fork_and_knife", "yum", "weary", "tired_face",
                                            "poop", "100" };
const char *const DEFAULT_REACTION_EMOJIS[] = { "100", "100", "thumbsup", "thumbsup",
                                                "thumbsdown", "thumbsdown", "heart", "point_up",
                                                "eyes", "weary", "laughing", "white_check_mark",
                                                "x" };
const char *const QUICK_REACTION_FALLBACKS[] = { "100", "laughing", "sparkling_heart" };

QString normalize(const QString &text)
{
    QString result = text.toLower();
    result.remove(' ');
    result.remove('_');
    return result;
}

struct QueryMatcher
{
    QString normalized;
    QString lowered;
    QRegularExpression prefix;
    QRegularExpression wordBoundary;

    explicit QueryMatcher(const QString &query)
        : normalized(normalize(query)),
          lowered(query.toLower()),
          prefix("^" + QRegularExpression::escape(lowered), QRegularExpression::CaseInsensitiveOption),
          wordBoundary("(^|_|[A-Z])" + QRegularExpression::escape(lowered) + "s?([A-Z]|_|$)")
    {
    }

    [[nodiscard]] bool matches(const QString &normalizedName) const
    {
        return normalizedName.contains(normalized);
    }

    [[nodiscard]] double baseWeight(const QString &name) const
    {
        const QString lowerName = name.toLower();
        double weight = 1;
        if (lowerName == lowered)
            weight += 4;

        const bool mixedCase = lowerName != name;
        if (wordBoundary.match(lowerName).hasMatch() || (mixedCase && wordBoundary.match(name).hasMatch()))
            weight += 2;
        if (prefix.match(name).hasMatch())
            weight += 1;
        return weight;
    }
};

QHash<QString, FrecencyTracker::Entry> toHistory(const Proto::EmojiFrecency &frecency)
{
    QHash<QString, FrecencyTracker::Entry> history;
    for (auto it = frecency.emojis.constBegin(); it != frecency.emojis.constEnd(); ++it) {
        FrecencyTracker::Entry entry;
        entry.totalUses = it->totalUses;
        entry.recentUses = it->recentUses;
        entry.score = it->score;
        history.insert(it.key(), entry);
    }
    return history;
}

Proto::EmojiFrecency toFrecency(const QHash<QString, FrecencyTracker::Entry> &history)
{
    Proto::EmojiFrecency frecency;
    for (auto it = history.constBegin(); it != history.constEnd(); ++it) {
        Proto::FrecencyItem item;
        item.totalUses = it->totalUses;
        item.recentUses = it->recentUses;
        item.frecency = it->frecency;
        item.score = static_cast<int32_t>(it->score);
        frecency.emojis.insert(it.key(), item);
    }
    return frecency;
}

} // namespace

EmojiManager::EmojiManager(Discord::Client *client, ClientInstance *instance, QObject *parent)
    : QObject(parent),
      client(client),
      instance(instance),
      chatTracker(FrecencyTracker::chatEmojiOptions()),
      reactionTracker(FrecencyTracker::reactionEmojiOptions())
{
    syncTimer.setSingleShot(true);
    connect(&syncTimer, &QTimer::timeout, this, &EmojiManager::onSyncTimer);
}

uint64_t EmojiManager::nowMs()
{
    return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
}

QString EmojiManager::unicodeKey(const QString &surrogates)
{
    const UnicodeEmoji *base = UnicodeEmojiIndex::instance().convertSurrogateToBase(surrogates);
    return base ? UnicodeEmojiIndex::primaryName(*base) : QString();
}

void EmojiManager::setGuildEmojis(Snowflake guildId, const QList<Discord::Emoji> &emojis)
{
    if (!guildId.isValid())
        return;

    guildEmojis.insert(guildId, emojis);
    customCandidatesDirty = true;
}

void EmojiManager::removeGuild(Snowflake guildId)
{
    if (guildEmojis.remove(guildId) > 0)
        customCandidatesDirty = true;
}

void EmojiManager::onGuildEmojisUpdated(const Discord::GuildEmojisUpdate &event)
{
    if (!event.guildId.hasValue())
        return;
    setGuildEmojis(event.guildId.get(), event.emojis.get());
}

const QList<EmojiManager::CustomEmoji> &EmojiManager::customEmojis()
{
    if (!customCandidatesDirty)
        return customCandidates;

    customCandidates.clear();
    customCandidateIndex.clear();
    customCandidatesDirty = false;

    // disambiguation follows guild order
    QList<Snowflake> guildIds = guildEmojis.keys();
    std::sort(guildIds.begin(), guildIds.end());

    const UnicodeEmojiIndex &index = UnicodeEmojiIndex::instance();
    QHash<QString, int> nameCounts;

    for (Snowflake guildId : guildIds) {
        const auto it = guildEmojis.constFind(guildId);
        if (it == guildEmojis.constEnd())
            continue;

        QList<Discord::Emoji> emojis = it.value();
        std::sort(emojis.begin(), emojis.end(),
                  [](const Discord::Emoji &a, const Discord::Emoji &b) {
                      return a.name.get() < b.name.get();
                  });

        for (const Discord::Emoji &emoji : emojis) {
            if (emoji.isUnicode() || !emoji.id.get().isValid() || !emoji.name.hasValue())
                continue;

            CustomEmoji custom;
            custom.emoji = emoji;
            custom.guildId = guildId;
            custom.originalName = emoji.name.get();

            auto countIt = nameCounts.find(custom.originalName);
            if (countIt == nameCounts.end()) {
                const UnicodeEmoji *shadowed = index.byName(custom.originalName);
                const bool takenByUnicode =
                        shadowed && UnicodeEmojiIndex::primaryName(*shadowed) == custom.originalName;
                countIt = nameCounts.insert(custom.originalName, takenByUnicode ? 1 : 0);
            }

            const int taken = countIt.value()++;
            custom.disambiguatedName = taken > 0
                                               ? custom.originalName + "~" + QString::number(taken)
                                               : custom.originalName;
            custom.normalizedName = normalize(custom.disambiguatedName);

            customCandidateIndex.insert(emoji.id.get(), customCandidates.size());
            customCandidates.append(custom);
        }
    }

    return customCandidates;
}

const QList<EmojiManager::UnicodeCandidate> &EmojiManager::unicodeCandidates()
{
    if (!unicodeCandidateCache.isEmpty())
        return unicodeCandidateCache;

    for (const UnicodeEmoji *emoji : UnicodeEmojiIndex::instance().topLevel()) {
        UnicodeCandidate candidate;
        candidate.emoji = emoji;
        candidate.normalizedNames.reserve(emoji->names.size() + emoji->keywords.size());
        for (const QString &name : emoji->names)
            candidate.normalizedNames.append(normalize(name));
        for (const QString &keyword : emoji->keywords)
            candidate.normalizedNames.append(normalize(keyword));
        unicodeCandidateCache.append(candidate);
    }
    return unicodeCandidateCache;
}

bool EmojiManager::isUsable(const CustomEmoji &custom, UsabilityContext &context) const
{
    const Discord::Emoji &emoji = custom.emoji;
    if (emoji.available.hasValue() && !emoji.available.get())
        return false;

    if (emoji.roles.hasValue() && !emoji.roles.get().isEmpty()) {
        auto rolesIt = context.rolesByGuild.find(custom.guildId);
        if (rolesIt == context.rolesByGuild.end()) {
            QSet<Snowflake> ids;
            for (const Discord::Role &role :
                 instance->getMemberRolesSorted(custom.guildId, context.selfId))
                ids.insert(role.id.get());
            rolesIt = context.rolesByGuild.insert(custom.guildId, ids);
        }

        bool hasRole = false;
        for (Snowflake roleId : emoji.roles.get()) {
            if (rolesIt->contains(roleId)) {
                hasRole = true;
                break;
            }
        }
        if (!hasRole)
            return false;
    }

    const bool inGuild = context.channelGuildId.isValid();
    const bool external = !inGuild || context.channelGuildId != custom.guildId;
    if (external) {
        if (inGuild) {
            if (!context.externalEmojisAllowed)
                context.externalEmojisAllowed = instance->permissions()->hasChannelPermission(context.selfId, context.channelId, Discord::Permission::USE_EXTERNAL_EMOJIS);
            if (!*context.externalEmojisAllowed)
                return false;
        }

        const bool managed = emoji.managed.hasValue() && emoji.managed.get();
        if (!managed && !context.premium)
            return false;
    }

    const bool animated = emoji.animated.hasValue() && emoji.animated.get();
    return !animated || context.premium;
}

EmojiManager::UsabilityContext EmojiManager::usabilityIn(Snowflake channelId) const
{
    UsabilityContext usability;
    usability.channelId = channelId;
    usability.selfId = client->getMe().id.get();
    usability.premium = client->isPremium();
    usability.channelGuildId = client->getGuildIdForChannel(channelId);
    return usability;
}

QList<EmojiManager::RankedEmoji> EmojiManager::rankedMatches(const QString &query, Snowflake channelId, FrecencyTracker &tracker)
{
    const QueryMatcher matcher(query);
    if (matcher.normalized.isEmpty())
        return {};

    tracker.refresh(nowMs());
    UsabilityContext usability = usabilityIn(channelId);

    struct Scored
    {
        double weight = 0;
        QString name;
        RankedEmoji emoji;
    };
    QList<Scored> scored;

    // scale by score
    auto weigh = [&tracker, &matcher](const QString &name, const QString &frecencyKey) {
        double weight = matcher.baseWeight(name);
        if (const std::optional<double> score = tracker.score(frecencyKey))
            weight *= *score / 100.0;
        return weight;
    };

    for (const UnicodeCandidate &candidate : unicodeCandidates()) {
        bool matched = false;
        for (const QString &name : candidate.normalizedNames) {
            if (matcher.matches(name)) {
                matched = true;
                break;
            }
        }
        if (!matched)
            continue;

        const QString primaryName = UnicodeEmojiIndex::primaryName(*candidate.emoji);
        scored.append({ weigh(primaryName, primaryName), primaryName, { candidate.emoji, nullptr } });
    }

    for (const CustomEmoji &custom : customEmojis()) {
        if (!matcher.matches(custom.normalizedName))
            continue;
        if (!isUsable(custom, usability))
            continue;

        scored.append({ weigh(custom.disambiguatedName, custom.emoji.id.get().toString()), custom.disambiguatedName, { nullptr, &custom } });
    }

    std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
        if (a.weight != b.weight)
            return a.weight > b.weight;
        return a.name < b.name;
    });

    QList<RankedEmoji> ranked;
    ranked.reserve(scored.size());
    for (const Scored &entry : scored)
        ranked.append(entry.emoji);
    return ranked;
}

QList<EmojiMatch> EmojiManager::search(const QString &query, Snowflake channelId, int maxResults)
{
    const QList<RankedEmoji> ranked = rankedMatches(query, channelId, chatTracker);

    QList<EmojiMatch> results;
    const int count = std::min<int>(ranked.size(), std::max(maxResults, 0));
    results.reserve(count);
    for (int i = 0; i < count; i++) {
        const RankedEmoji &emoji = ranked.at(i);
        EmojiMatch match;
        if (emoji.unicode) {
            match.insertText = ":" + UnicodeEmojiIndex::primaryName(*emoji.unicode) + ":";
            match.displayLabel = match.insertText;
            match.surrogates = emoji.unicode->surrogates;
        } else {
            const CustomEmoji &custom = *emoji.custom;
            const Snowflake id = custom.emoji.id.get();
            match.insertText = (custom.emoji.isAnimated() ? "<a:" : "<:") + custom.originalName + ":" + id.toString() + ">";
            match.displayLabel = ":" + custom.disambiguatedName + ":";
            match.customId = id;
            match.imageUrl = custom.emoji.getImageUrl();
        }
        results.append(match);
    }
    return results;
}

PickerEmoji EmojiManager::toPickerEmoji(const UnicodeEmoji &emoji)
{
    PickerEmoji picker;
    picker.name = UnicodeEmojiIndex::primaryName(emoji);
    picker.surrogates = emoji.surrogates;
    return picker;
}

PickerEmoji EmojiManager::toPickerEmoji(const CustomEmoji &custom)
{
    PickerEmoji picker;
    picker.name = custom.disambiguatedName;
    picker.customId = custom.emoji.id.get();
    picker.customName = custom.originalName;
    picker.guildId = custom.guildId;
    picker.animated = custom.emoji.isAnimated();
    return picker;
}

QList<PickerEmoji> EmojiManager::searchReactions(const QString &query, Snowflake channelId)
{
    const QList<RankedEmoji> ranked = rankedMatches(query, channelId, reactionTracker);

    QList<PickerEmoji> results;
    results.reserve(ranked.size());
    for (const RankedEmoji &emoji : ranked)
        results.append(emoji.unicode ? toPickerEmoji(*emoji.unicode) : toPickerEmoji(*emoji.custom));
    return results;
}

QList<PickerEmoji> EmojiManager::frequentReactions(Snowflake channelId)
{
    const QList<CustomEmoji> &customs = customEmojis();
    const UnicodeEmojiIndex &index = UnicodeEmojiIndex::instance();
    UsabilityContext usability = usabilityIn(channelId);

    QList<PickerEmoji> frequent;
    for (const QString &key : reactionTracker.frequently(nowMs())) {
        if (const UnicodeEmoji *emoji = index.byName(key)) {
            frequent.append(toPickerEmoji(*emoji));
            continue;
        }

        const auto it = customCandidateIndex.constFind(Snowflake(key.toULongLong()));
        if (it != customCandidateIndex.constEnd() && isUsable(customs.at(it.value()), usability))
            frequent.append(toPickerEmoji(customs.at(it.value())));
    }
    return frequent;
}

QList<PickerEmoji> EmojiManager::quickReactions(Snowflake channelId, int count)
{
    QList<PickerEmoji> quick = frequentReactions(channelId);

    const UnicodeEmojiIndex &index = UnicodeEmojiIndex::instance();
    for (const char *name : QUICK_REACTION_FALLBACKS) {
        if (quick.size() >= count)
            break;
        const UnicodeEmoji *emoji = index.byName(QString::fromLatin1(name));
        if (!emoji)
            continue;

        const PickerEmoji fallback = toPickerEmoji(*emoji);
        const bool alreadyListed = std::any_of(quick.cbegin(), quick.cend(), [&fallback](const PickerEmoji &listed) { return listed.sameEmoji(fallback); });
        if (!alreadyListed)
            quick.append(fallback);
    }

    return quick.mid(0, count);
}

QList<Snowflake> EmojiManager::guildsInSidebarOrder() const
{
    QList<Snowflake> placedBySettings;
    const std::optional<Proto::GuildFolders> &folders = client->getSettings().guildFolders;
    if (folders && !folders->folders.isEmpty()) {
        for (const Proto::GuildFolder &folder : folders->folders)
            placedBySettings.append(folder.guildIds);
    } else if (folders) {
        placedBySettings = folders->guildPositions;
    }

    const QSet<Snowflake> placed(placedBySettings.cbegin(), placedBySettings.cend());
    QList<Snowflake> unplacedNewestFirst;
    for (auto it = guildEmojis.constBegin(); it != guildEmojis.constEnd(); ++it) {
        if (!placed.contains(it.key()))
            unplacedNewestFirst.append(it.key());
    }
    std::sort(unplacedNewestFirst.begin(), unplacedNewestFirst.end(), [](Snowflake a, Snowflake b) { return b < a; });

    return unplacedNewestFirst + placedBySettings;
}

QList<PickerSection> EmojiManager::reactionPickerSections(Snowflake channelId)
{
    QList<PickerSection> sections;

    QList<PickerEmoji> frequent = frequentReactions(channelId);
    if (!frequent.isEmpty())
        sections.append({ QString::fromLatin1(PickerSection::RecentId), tr("Frequently Used"), Snowflake(), QUrl(), std::move(frequent) });

    UsabilityContext usability = usabilityIn(channelId);
    QHash<Snowflake, QList<PickerEmoji>> usableByGuild;
    for (const CustomEmoji &custom : customEmojis()) {
        if (isUsable(custom, usability))
            usableByGuild[custom.guildId].append(toPickerEmoji(custom));
    }

    QList<Snowflake> guildOrder = guildsInSidebarOrder();
    if (usability.channelGuildId.isValid())
        guildOrder.prepend(usability.channelGuildId);

    for (Snowflake guildId : guildOrder) {
        const auto it = usableByGuild.find(guildId);
        if (it == usableByGuild.end())
            continue;

        PickerSection section;
        section.id = guildId.toString();
        section.guildId = guildId;
        if (const std::optional<Discord::Guild> guild = instance->getGuild(guildId)) {
            section.title = guild->name.valueOr(QString());
            if (guild->icon.hasValue() && !guild->icon.get().isEmpty())
                section.guildIconUrl = Discord::Cdn::guildIcon(guildId, guild->icon.get(), 64);
        }
        section.emojis = std::move(it.value());
        sections.append(section);
        usableByGuild.erase(it);
    }

    for (const UnicodeEmojiCategory &category : UnicodeEmojiIndex::instance().categories()) {
        PickerSection section;
        section.id = category.id;
        section.title = unicodeCategoryTitle(category.id);
        section.emojis.reserve(category.emojis.size());
        for (const UnicodeEmoji *emoji : category.emojis)
            section.emojis.append(toPickerEmoji(*emoji));
        sections.append(section);
    }

    return sections;
}

QString EmojiManager::unicodeCategoryTitle(const QString &categoryId)
{
    if (categoryId == QLatin1String("people"))
        return tr("People");
    if (categoryId == QLatin1String("nature"))
        return tr("Nature");
    if (categoryId == QLatin1String("food"))
        return tr("Food");
    if (categoryId == QLatin1String("activity"))
        return tr("Activities");
    if (categoryId == QLatin1String("travel"))
        return tr("Travel");
    if (categoryId == QLatin1String("objects"))
        return tr("Objects");
    if (categoryId == QLatin1String("symbols"))
        return tr("Symbols");
    if (categoryId == QLatin1String("flags"))
        return tr("Flags");
    return categoryId;
}

void EmojiManager::trackUsage(const QString &key, bool reaction)
{
    if (key.isEmpty())
        return;

    const uint64_t timestamp = nowMs();
    // a reaction counts towards both
    if (reaction)
        reactionTracker.track(key, timestamp);
    chatTracker.track(key, timestamp);

    if (pendingUsages.size() >= MAX_PENDING_USAGES)
        pendingUsages.removeFirst();
    pendingUsages.append({ key, timestamp, reaction });
}

void EmojiManager::trackMessageEmojis(const QString &content)
{
    if (content.isEmpty())
        return;

    auto customIt = customEmojiTagRe().globalMatch(content);
    while (customIt.hasNext())
        trackUsage(customIt.next().captured(2), false);

    trackSurrogates(content);
}

void EmojiManager::trackSurrogates(const QString &content)
{
    for (const QString &sequence : extractEmojiSequences(content))
        trackUsage(unicodeKey(sequence), false);
}

// `reactionEmoji` is in REST form
void EmojiManager::trackReaction(const QString &reactionEmoji)
{
    const int idSeparator = reactionEmoji.lastIndexOf(':');
    if (idSeparator == -1)
        trackUsage(unicodeKey(reactionEmoji), true);
    else
        trackUsage(reactionEmoji.mid(idSeparator + 1), true);
}

void EmojiManager::onReady()
{
    fetchSettings();
}

void EmojiManager::fetchSettings()
{
    QPointer<EmojiManager> self(this);
    client->fetchFrecencySettings([this, self](const Result<Proto::FrecencyUserSettings> &result) {
        if (!self)
            return;

        if (!result.success()) {
            qCWarning(LogCore) << "EmojiManager: frecency settings fetch failed:" << result.error;
            syncTimer.start(FETCH_RETRY_MS);
            return;
        }

        adoptSettings(*result.value, false);

        replayPendingUsages();
        seedDefaultsIfEmpty();

        loaded = true;
        syncTimer.start(FIRST_SAVE_MS + QRandomGenerator::global()->bounded(FIRST_SAVE_JITTER_MS));

        qCInfo(LogCore) << "EmojiManager: loaded frecency for" << instance->accountId() << "-"
                        << chatTracker.usageHistory().size() << "chat,"
                        << reactionTracker.usageHistory().size() << "reaction entries";
    });
}

void EmojiManager::adoptSettings(const Proto::FrecencyUserSettings &settings, bool partial)
{
    if (settings.dataVersion)
        dataVersion = settings.dataVersion;

    auto apply = [partial](FrecencyTracker &tracker,
                           const std::optional<Proto::EmojiFrecency> &frecency) {
        if (!frecency) {
            if (!partial)
                tracker.reset();
            return;
        }

        QHash<QString, FrecencyTracker::Entry> history = toHistory(*frecency);
        if (partial) {
            QHash<QString, FrecencyTracker::Entry> merged = tracker.usageHistory();
            for (auto it = history.constBegin(); it != history.constEnd(); ++it)
                merged.insert(it.key(), it.value());
            history = merged;
        }
        tracker.overwriteHistory(history);
    };

    apply(chatTracker, settings.emojiFrecency);
    apply(reactionTracker, settings.emojiReactionFrecency);
}

void EmojiManager::replayPendingUsages()
{
    for (const PendingUsage &usage : pendingUsages) {
        if (usage.reaction)
            reactionTracker.track(usage.key, usage.timestamp);
        chatTracker.track(usage.key, usage.timestamp);
    }
}

void EmojiManager::seedDefaultsIfEmpty()
{
    if (chatTracker.usageHistory().isEmpty()) {
        for (const char *name : DEFAULT_CHAT_EMOJIS)
            chatTracker.track(QString::fromLatin1(name));
    }

    if (reactionTracker.usageHistory().isEmpty()) {
        for (const char *name : DEFAULT_REACTION_EMOJIS)
            reactionTracker.track(QString::fromLatin1(name));
    }
}

void EmojiManager::onUserSettingsProtoUpdated(const Discord::UserSettingsProtoUpdate &event)
{
    if (!event.type.hasValue() || event.type.get() != Discord::UserSettingsProtoType::FRECENCY || !event.proto.hasValue())
        return;

    const QByteArray decoded = QByteArray::fromBase64(event.proto.get().toUtf8());
    if (decoded.isEmpty())
        return;

    Proto::ProtoReader reader(decoded);
    const Proto::FrecencyUserSettings settings = Proto::FrecencyUserSettings::fromProto(reader);

    const bool partial = event.partial.hasValue() && event.partial.get();
    adoptSettings(settings, partial);

    if (!partial)
        replayPendingUsages();

    qCInfo(LogCore) << "EmojiManager: frecency settings update, partial" << partial << "dataVersion"
                    << (dataVersion ? int(*dataVersion) : -1);
}

void EmojiManager::onSyncTimer()
{
    if (!loaded) {
        fetchSettings();
        return;
    }

    savePendingUsage();
    syncTimer.start(PERIODIC_SAVE_MS + QRandomGenerator::global()->bounded(PERIODIC_SAVE_JITTER_MS));
}

void EmojiManager::savePendingUsage()
{
    if (!loaded || pendingUsages.isEmpty() || saveInFlight)
        return;

    const uint64_t now = nowMs();
    chatTracker.compute(now);
    reactionTracker.compute(now);

    Proto::FrecencyUserSettings partial;
    partial.emojiFrecency = toFrecency(FrecencyTracker::capForPersistence(chatTracker.usageHistory(), PERSISTED_ENTRY_LIMIT));
    partial.emojiReactionFrecency = toFrecency(FrecencyTracker::capForPersistence(reactionTracker.usageHistory(), PERSISTED_ENTRY_LIMIT));

    QList<PendingUsage> inFlight;
    inFlight.swap(pendingUsages);
    saveInFlight = true;

    QPointer<EmojiManager> self(this);
    client->patchFrecencySettings(
            partial.toProtoPartial(), dataVersion,
            [this, self, inFlight](const Discord::Client::FrecencyPatchResult &result) {
                if (!self)
                    return;

                saveInFlight = false;

                if (!result.success || result.outOfDate) {
                    pendingUsages = inFlight + pendingUsages;

                    if (result.outOfDate) {
                        qCInfo(LogCore) << "EmojiManager: frecency save out of date, refetching";
                        fetchSettings();
                    } else if (result.rateLimited) {
                        qCWarning(LogCore) << "EmojiManager: frecency save rate limited, retrying";
                        syncTimer.start(RATE_LIMIT_BACKOFF_MS);
                    } else {
                        qCWarning(LogCore) << "EmojiManager: frecency save failed:" << result.error;
                    }
                    return;
                }

                if (result.settings) {
                    adoptSettings(*result.settings, false);
                    replayPendingUsages();
                }

                qCInfo(LogCore) << "EmojiManager: saved" << inFlight.size() << "emoji usages";
            });
}

} // namespace Core
} // namespace Acheron
