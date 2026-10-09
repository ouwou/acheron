#include "Client.hpp"

#include <QDebug>
#include <QGuiApplication>
#include <QJsonObject>
#include <QPointer>
#include <QtMath>

#include "ApiError.hpp"
#include "Enums.hpp"
#include "Core/Logging.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/UserSettings.hpp"

namespace Acheron {
namespace Discord {

namespace {

Proto::GuildFolders guildFoldersFromLegacy(const QList<GuildFolderEntry> &entries)
{
    Proto::GuildFolders result;
    result.folders.reserve(entries.size());
    for (const auto &entry : entries) {
        Proto::GuildFolder folder;
        folder.guildIds = entry.guildIds.get();
        if (entry.id.hasValue())
            folder.id = entry.id.get();
        if (entry.name.hasValue())
            folder.name = entry.name.get();
        if (entry.color.hasValue())
            folder.color = static_cast<uint64_t>(entry.color.get());
        result.folders.append(folder);
    }
    return result;
}

QString frecencySettingsEndpoint()
{
    return "/users/@me/settings-proto/" + QString::number(int(UserSettingsProtoType::FRECENCY));
}

std::optional<Proto::FrecencyUserSettings> decodeFrecencySettings(const QJsonObject &obj)
{
    const QByteArray proto = QByteArray::fromBase64(obj.value("settings").toString().toUtf8());
    if (proto.isEmpty())
        return std::nullopt;

    Proto::ProtoReader reader(proto);
    return Proto::FrecencyUserSettings::fromProto(reader);
}

QString guildEndpoint(Snowflake guildId, const QString &path = {})
{
    return "/guilds/" + QString::number(guildId) + path;
}

QString memberEndpoint(Snowflake guildId, Snowflake userId)
{
    return guildEndpoint(guildId, "/members/" + QString::number(userId));
}

QString roleEndpoint(Snowflake guildId, Snowflake roleId, const QString &path = {})
{
    return guildEndpoint(guildId, "/roles/" + QString::number(roleId) + path);
}

QJsonArray snowflakeArray(const QList<Snowflake> &ids)
{
    QJsonArray array;
    for (Snowflake id : ids)
        array.append(QString::number(id));
    return array;
}

HttpCallback actionHandler(const char *what, Client::ActionCallback callback)
{
    return [what, callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            const ApiError error = ApiError::fromResponse(response);
            qCWarning(LogDiscord) << what << "failed with status" << response.statusCode << ":" << error.message;
            if (callback)
                callback(Core::Result<void>::makeError(error.message, error.code));
            return;
        }
        if (callback)
            callback(Core::Result<void>::makeOk());
    };
}

template <typename T, typename Parse>
HttpCallback valueHandler(const char *what, Client::ResultCallback<T> callback, Parse parse)
{
    return [what, callback = std::move(callback), parse = std::move(parse)](const HttpResponse &response) {
        if (!response.success) {
            const ApiError error = ApiError::fromResponse(response);
            qCWarning(LogDiscord) << what << "failed with status" << response.statusCode << ":" << error.message;
            callback(Core::Result<T>::makeError(error.message, error.code));
            return;
        }
        callback(Core::Result<T>::makeOk(parse(QJsonDocument::fromJson(response.body))));
    };
}

template <typename T>
QList<T> parseArray(const QJsonDocument &doc)
{
    QList<T> list;
    for (const QJsonValue &value : doc.array())
        list.append(T::fromJson(value.toObject()));
    return list;
}

QByteArray originalMd5Header(const QMap<QString, QByteArray> &md5s)
{
    QList<QByteArray> parts;
    for (auto it = md5s.constBegin(); it != md5s.constEnd(); ++it)
        parts.append(it.key().toLower().toUtf8() + "=\"" + it.value() + '"');
    return parts.join(", ");
}

QString isoTimestamp(const QDateTime &time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

} // namespace

Client::Client(const QString &token, const QString &gatewayUrl, const QString &baseUrl,
               const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver, QObject *parent)
    : QObject(parent), token(token), baseUrl(baseUrl), proxyConfig(proxy)
{
    gateway = new Gateway(token, gatewayUrl, identity, proxy, this);
    httpClient = new HttpClient(baseUrl, token, identity, proxy, captchaResolver, this);

    heartbeatSessionTimer = new QTimer(this);
    heartbeatSessionTimer->setInterval(15 * 60 * 1000);
    connect(heartbeatSessionTimer, &QTimer::timeout, this, &Client::onHeartbeatSessionTimer);

    appFocused = !qGuiApp || qGuiApp->applicationState() == Qt::ApplicationActive;
    if (qGuiApp) {
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            appFocused = state == Qt::ApplicationActive;
            updateActiveState();
        });
    }
    updateActiveState();

    connect(gateway, &Gateway::connected, this, &Client::onConnected);
    connect(gateway, &Gateway::disconnected, this, &Client::onDisconnected);
    connect(gateway, &Gateway::finished, this,
            [this] { setState(Core::ConnectionState::Disconnected); });

    connect(gateway, &Gateway::gatewayReady, this, &Client::onGatewayReady);
    connect(gateway, &Gateway::gatewayReadySupplemental, this, &Client::onGatewayReadySupplemental);
    connect(gateway, &Gateway::gatewayMessageCreate, this, &Client::onGatewayMessageCreate);
    connect(gateway, &Gateway::gatewayMessageUpdate, this, &Client::onGatewayMessageUpdate);
    connect(gateway, &Gateway::gatewayMessageDelete, this, &Client::onGatewayMessageDelete);
    connect(gateway, &Gateway::gatewayTypingStart, this, &Client::typingStart);
    connect(gateway, &Gateway::gatewayChannelCreate, this, &Client::onGatewayChannelCreate);
    connect(gateway, &Gateway::gatewayChannelUpdate, this, &Client::onGatewayChannelUpdate);
    connect(gateway, &Gateway::gatewayChannelDelete, this, &Client::onGatewayChannelDelete);
    connect(gateway, &Gateway::gatewayThreadCreate, this, &Client::onGatewayThreadCreate);
    connect(gateway, &Gateway::gatewayThreadUpdate, this, &Client::onGatewayThreadUpdate);
    connect(gateway, &Gateway::gatewayThreadDelete, this, &Client::onGatewayThreadDelete);
    connect(gateway, &Gateway::gatewayThreadListSync, this, &Client::onGatewayThreadListSync);
    connect(gateway, &Gateway::gatewayThreadMemberUpdate, this, &Client::threadMemberUpdated);
    connect(gateway, &Gateway::gatewayThreadMembersUpdate, this, &Client::threadMembersUpdated);
    connect(gateway, &Gateway::gatewayForumUnreads, this, &Client::forumUnreads);
    connect(gateway, &Gateway::gatewayGuildCreate, this, &Client::onGatewayGuildCreate);
    connect(gateway, &Gateway::gatewayGuildUpdate, this, &Client::onGatewayGuildUpdate);
    connect(gateway, &Gateway::gatewayGuildDelete, this, &Client::onGatewayGuildDelete);
    connect(gateway, &Gateway::gatewayGuildMembersChunk, this, &Client::guildMembersChunk);
    connect(gateway, &Gateway::gatewayGuildMemberAdd, this, &Client::guildMemberAdded);
    connect(gateway, &Gateway::gatewayGuildMemberUpdate, this, &Client::guildMemberUpdated);
    connect(gateway, &Gateway::gatewayGuildMemberRemove, this, &Client::guildMemberRemoved);
    connect(gateway, &Gateway::gatewayGuildRoleCreate, this, &Client::onGatewayGuildRoleCreate);
    connect(gateway, &Gateway::gatewayGuildRoleUpdate, this, &Client::onGatewayGuildRoleUpdate);
    connect(gateway, &Gateway::gatewayGuildRoleDelete, this, &Client::onGatewayGuildRoleDelete);
    connect(gateway, &Gateway::gatewayGuildEmojisUpdate, this, &Client::guildEmojisUpdated);
    connect(gateway, &Gateway::gatewayGuildStickersUpdate, this, &Client::guildStickersUpdated);
    connect(gateway, &Gateway::gatewayGuildBanAdd, this, &Client::guildBanAdded);
    connect(gateway, &Gateway::gatewayGuildBanRemove, this, &Client::guildBanRemoved);
    connect(gateway, &Gateway::gatewayGuildPruneUpdate, this, &Client::guildPruneUpdated);
    connect(gateway, &Gateway::gatewayMessageAck, this, &Client::messageAcked);
    connect(gateway, &Gateway::gatewayMessageReactionAdd, this, &Client::messageReactionAdd);
    connect(gateway, &Gateway::gatewayMessageReactionAddMany, this, &Client::messageReactionAddMany);
    connect(gateway, &Gateway::gatewayMessageReactionRemove, this, &Client::messageReactionRemove);
    connect(gateway, &Gateway::gatewayMessageReactionRemoveAll, this, &Client::messageReactionRemoveAll);
    connect(gateway, &Gateway::gatewayMessageReactionRemoveEmoji, this, &Client::messageReactionRemoveEmoji);
    connect(gateway, &Gateway::gatewayUserGuildSettingsUpdate, this, &Client::userGuildSettingsUpdated);
    connect(gateway, &Gateway::gatewayNotificationSettingsUpdate, this, &Client::notificationSettingsUpdated);
    connect(gateway, &Gateway::gatewayGuildMemberListUpdate, this, &Client::guildMemberListUpdate);
    connect(gateway, &Gateway::gatewayVoiceStateUpdate, this, &Client::voiceStateUpdated);
    connect(gateway, &Gateway::gatewayVoiceServerUpdate, this, &Client::voiceServerUpdated);
    connect(gateway, &Gateway::gatewayRelationshipAdd, this, &Client::relationshipAdded);
    connect(gateway, &Gateway::gatewayRelationshipUpdate, this, &Client::relationshipUpdated);
    connect(gateway, &Gateway::gatewayRelationshipRemove, this, &Client::relationshipRemoved);
    connect(gateway, &Gateway::gatewayUserNoteUpdate, this, &Client::userNoteUpdated);
    connect(gateway, &Gateway::gatewayUserSettingsProtoUpdate, this, &Client::onGatewayUserSettingsProtoUpdate);
    connect(gateway, &Gateway::gatewayPresenceUpdate, this, &Client::presenceUpdated);
    connect(gateway, &Gateway::gatewayPresencesReplace, this, &Client::presencesReplaced);
    connect(gateway, &Gateway::gatewaySessionsReplace, this, &Client::sessionsReplaced);
    connect(gateway, &Gateway::reconnecting, this, [this](int attempt, int maxAttempts) {
        setState(Core::ConnectionState::Connecting);
        emit reconnecting(attempt, maxAttempts);
    });
}

Client::~Client()
{
    // join and delete the http clients thread before everything else
    delete httpClient;
    httpClient = nullptr;
}

void Client::start()
{
    setState(Core::ConnectionState::Connecting);
    gateway->start();
}

void Client::stop()
{
    if (!gateway->isRunning()) {
        setState(Core::ConnectionState::Disconnected);
        return;
    }

    setState(Core::ConnectionState::Disconnecting);
    gateway->stop();
}

[[nodiscard]] Core::ConnectionState Client::getState() const
{
    return state;
}

void Client::fetchMessages(Snowflake channelId, QUrlQuery query, int limit, MessagesCallback callback)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/messages";
    query.addQueryItem("limit", QString::number(limit));

    httpClient->get(endpoint, query, [callback](const HttpResponse &response) {
        if (!response.success) {
            callback({ {}, response.error });
            return;
        }

        QList<Message> results;
        QJsonArray arr = QJsonDocument::fromJson(response.body).array();
        for (const QJsonValue &val : arr)
            results.append(Message::fromJson(val.toObject()));

        callback({ results });
    });
}

void Client::fetchLatestMessages(Snowflake channelId, int limit, MessagesCallback callback)
{
    fetchMessages(channelId, {}, limit, std::move(callback));
}

void Client::fetchHistory(Snowflake channelId, Snowflake beforeId, int limit,
                          MessagesCallback callback)
{
    QUrlQuery query;
    query.addQueryItem("before", QString::number(beforeId));
    fetchMessages(channelId, query, limit, std::move(callback));
}

void Client::fetchMessagesAfter(Snowflake channelId, Snowflake afterId, int limit, MessagesCallback callback)
{
    QUrlQuery query;
    query.addQueryItem("after", QString::number(afterId));
    fetchMessages(channelId, query, limit, std::move(callback));
}

void Client::fetchMessagesAround(Snowflake channelId, Snowflake messageId, int limit, MessagesCallback callback)
{
    QUrlQuery query;
    query.addQueryItem("around", QString::number(messageId));
    fetchMessages(channelId, query, limit, std::move(callback));
}

void Client::fetchUserProfile(Snowflake userId, Snowflake guildId, ProfileCallback callback)
{
    QString endpoint = "/users/" + QString::number(userId) + "/profile";
    QUrlQuery query;
    query.addQueryItem("type", "popout");
    query.addQueryItem("with_mutual_guilds", "true");
    query.addQueryItem("with_mutual_friends", "true");
    query.addQueryItem("with_mutual_friends_count", "false");
    if (guildId.isValid())
        query.addQueryItem("guild_id", QString::number(guildId));

    httpClient->get(endpoint, query, [userId, callback](const HttpResponse &response) {
        if (!response.success) {
            qCWarning(LogDiscord) << "Failed to fetch user profile for" << userId << ":"
                                  << response.error;
            callback({ {}, "Failed to fetch user profile: " + response.error });
            return;
        }

        UserProfile profile = UserProfile::fromJson(QJsonDocument::fromJson(response.body).object());
        callback({ profile });
    });
}

QString Client::applicationIconHash(Snowflake applicationId)
{
    if (!applicationId.isValid())
        return {};

    auto cached = applicationIcons.constFind(applicationId);
    if (cached != applicationIcons.constEnd())
        return cached.value();

    if (applicationIconRequests.contains(applicationId))
        return {};

    applicationIconRequests.insert(applicationId);

    QString endpoint = "/applications/" + QString::number(applicationId) + "/public";
    QPointer<Client> self(this);
    httpClient->get(endpoint, QUrlQuery(), [self, applicationId](const HttpResponse &response) {
        if (!self)
            return;

        self->applicationIconRequests.remove(applicationId);

        QString icon;
        if (response.success)
            icon = QJsonDocument::fromJson(response.body).object().value("icon").toString();
        else
            qCWarning(LogDiscord) << "Failed to fetch application" << applicationId << ":"
                                  << response.error;

        self->applicationIcons.insert(applicationId, icon);
        if (!icon.isEmpty())
            emit self->applicationIconResolved(applicationId);
    });

    return {};
}

void Client::refreshAttachmentUrls(const QList<QUrl> &urls, RefreshedUrlsCallback callback)
{
    QJsonArray requested;
    for (const QUrl &url : urls)
        requested.append(url.toString());

    QJsonObject payload;
    payload["attachment_urls"] = requested;

    httpClient->post("/attachments/refresh-urls",
                     payload,
                     [urls, callback](const HttpResponse &response) {
                         if (!response.success) {
                             qCWarning(LogDiscord) << "Failed to refresh attachment URLs:" << response.error;
                             callback(Core::Result<QHash<QUrl, QUrl>>::makeError(response.error));
                             return;
                         }

                         QHash<QUrl, QUrl> refreshed;
                         const QJsonArray entries =
                                 QJsonDocument::fromJson(response.body).object().value("refreshed_urls").toArray();

                         // matches pos
                         for (int i = 0; i < entries.size() && i < urls.size(); ++i) {
                             const QString fresh = entries[i].toObject().value("refreshed").toString();
                             if (!fresh.isEmpty())
                                 refreshed.insert(urls[i], QUrl(fresh));
                         }

                         callback(Core::Result<QHash<QUrl, QUrl>>::makeOk(refreshed));
                     });
}

void Client::setUserNote(Snowflake userId, const QString &note)
{
    QString endpoint = "/users/@me/notes/" + QString::number(userId);
    QJsonObject payload;
    payload["note"] = note;

    httpClient->put(endpoint, payload, [userId](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to set note for user" << userId << ":"
                                  << response.error;
    });
}

void Client::openDmChannel(Snowflake userId, DmChannelCallback callback)
{
    QJsonObject payload;
    payload["recipients"] = QJsonArray{ QString::number(userId) };

    httpClient->post("/users/@me/channels", payload, ContextProperties::empty(),
                     [userId, callback](const HttpResponse &response) {
                         if (!response.success) {
                             qCWarning(LogDiscord) << "Failed to open DM with user" << userId << ":" << response.error;
                             callback(Core::Result<ChannelCreate>::makeError(response.error));
                             return;
                         }

                         callback(Core::Result<ChannelCreate>::makeOk(ChannelCreate::fromJson(QJsonDocument::fromJson(response.body).object())));
                     });
}

namespace {

template <typename ResultT, typename ParseExtra>
void runThreadSearch(HttpClient *http, Core::Snowflake channelId, const QUrlQuery &query,
                     const QString &errorPrefix,
                     std::function<void(const Core::Result<ResultT> &)> callback,
                     ParseExtra parseExtra)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/threads/search";
    http->get(endpoint, query, [callback, parseExtra, errorPrefix](const HttpResponse &response) {
        if (response.statusCode == 202) {
            QJsonObject obj = QJsonDocument::fromJson(response.body).object();
            ResultT result;
            result.indexNotReady = true;
            result.retryAfterSeconds = qMax(1, qRound(obj.value("retry_after").toDouble(1.0)));
            callback(Core::Result<ResultT>::makeOk(result));
            return;
        }

        if (!response.success) {
            qCWarning(LogDiscord) << errorPrefix << response.error;
            callback(Core::Result<ResultT>::makeError(errorPrefix + " " + response.error));
            return;
        }

        QJsonObject obj = QJsonDocument::fromJson(response.body).object();
        ResultT result;
        result.hasMore = obj.value("has_more").toBool();
        for (const QJsonValue &val : obj.value("threads").toArray())
            result.threads.append(Channel::fromJson(val.toObject()));
        parseExtra(obj, result);
        callback(Core::Result<ResultT>::makeOk(result));
    });
}

} // namespace

void Client::searchForumThreads(Snowflake forumId, int offset, const QString &sortBy, ForumThreadsCallback callback)
{
    QUrlQuery query;
    query.addQueryItem("sort_by", sortBy);
    query.addQueryItem("sort_order", "desc");
    query.addQueryItem("limit", "25");
    query.addQueryItem("tag_setting", "match_some");
    if (offset > 0)
        query.addQueryItem("offset", QString::number(offset));

    runThreadSearch<ForumThreadSearchResult>(
            httpClient, forumId, query, QStringLiteral("Failed to search forum threads:"),
            std::move(callback), [](const QJsonObject &obj, ForumThreadSearchResult &result) {
                for (const QJsonValue &val : obj.value("first_messages").toArray()) {
                    Message msg = Message::fromJson(val.toObject());
                    if (msg.channelId.hasValue())
                        result.firstMessages.insert(msg.channelId.get(), msg);
                }
            });
}

void Client::joinThread(Snowflake threadId)
{
    QString endpoint = "/channels/" + QString::number(threadId) + "/thread-members/@me";
    httpClient->put(endpoint, QJsonObject{}, [threadId](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to join thread" << threadId << ":" << response.error;
    });
}

void Client::leaveThread(Snowflake threadId)
{
    QString endpoint = "/channels/" + QString::number(threadId) + "/thread-members/@me";
    httpClient->delete_(endpoint, [threadId](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to leave thread" << threadId << ":" << response.error;
    });
}

void Client::searchThreads(Snowflake channelId, bool archived, int offset, ThreadListCallback callback)
{
    QUrlQuery query;
    query.addQueryItem("archived", archived ? "true" : "false");
    query.addQueryItem("sort_by", "last_message_time");
    query.addQueryItem("sort_order", "desc");
    query.addQueryItem("limit", "25");
    if (offset > 0)
        query.addQueryItem("offset", QString::number(offset));

    runThreadSearch<ThreadListResult>(
            httpClient, channelId, query, QStringLiteral("Failed to search threads:"),
            std::move(callback), [](const QJsonObject &obj, ThreadListResult &result) {
                for (const QJsonValue &val : obj.value("members").toArray())
                    result.members.append(ThreadMember::fromJson(val.toObject()));
            });
}

void Client::createForumThread(Snowflake forumId, const QString &name,
                               const QList<Snowflake> &appliedTags, const QString &content,
                               const QString &nonce,
                               const QList<Core::PendingAttachment> &attachments,
                               ForumThreadCallback callback)
{
    QJsonObject message;
    message["content"] = content;

    if (attachments.isEmpty()) {
        postForumThread(forumId, name, appliedTags, message, callback);
        return;
    }

    auto state = std::make_shared<UploadState>();
    state->channelId = forumId;
    state->nonce = nonce;
    state->attachments = attachments;
    state->onUploaded = [this, forumId, name, appliedTags, message, callback](const QJsonArray &attachmentsJson) {
        QJsonObject withFiles = message;
        withFiles["attachments"] = attachmentsJson;
        postForumThread(forumId, name, appliedTags, withFiles, callback);
    };
    state->onFailed = [callback](const QString &error) {
        if (callback)
            callback(Core::Result<CreatedForumThread>::makeError("Failed to create forum post: " + error));
    };

    for (int i = 0; i < attachments.size(); i++) {
        state->uploadFilenames.append(QString());
        state->uploaded.append(false);
    }
    state->cancelFlag = std::make_shared<std::atomic<bool>>(false);
    activeUploads.insert(nonce, state);
    uploadAttachmentsAndSend(state);
}

void Client::postForumThread(Snowflake forumId, const QString &name,
                             const QList<Snowflake> &appliedTags,
                             const QJsonObject &message,
                             ForumThreadCallback callback)
{
    QJsonArray tags;
    for (Snowflake tag : appliedTags)
        tags.append(QString::number(tag));

    QJsonObject body;
    body["name"] = name;
    body["auto_archive_duration"] = 1440;
    body["applied_tags"] = tags;
    body["message"] = message;

    QString endpoint = "/channels/" + QString::number(forumId) + "/threads?use_nested_fields=true";
    httpClient->post(endpoint, body, [callback](const HttpResponse &response) {
        if (!response.success) {
            qCWarning(LogDiscord) << "Failed to create forum thread:" << response.error;
            if (callback)
                callback(Core::Result<CreatedForumThread>::makeError(
                        "Failed to create forum thread: " + response.error));
            return;
        }
        QJsonObject obj = QJsonDocument::fromJson(response.body).object();
        CreatedForumThread created;
        created.thread = Channel::fromJson(obj);
        if (obj.contains("message"))
            created.starterMessage = Message::fromJson(obj.value("message").toObject());
        if (callback)
            callback(Core::Result<CreatedForumThread>::makeOk(created));
    });
}

void Client::fetchForumPostData(Snowflake forumId, const QList<Snowflake> &threadIds,
                                ForumPostDataCallback callback)
{
    QJsonArray ids;
    for (Snowflake id : threadIds)
        ids.append(QString::number(id));

    QJsonObject body;
    body["thread_ids"] = ids;

    QString endpoint = "/channels/" + QString::number(forumId) + "/post-data";
    httpClient->post(endpoint, body, [callback](const HttpResponse &response) {
        if (!response.success) {
            qCWarning(LogDiscord) << "Failed to fetch forum post data:" << response.error;
            callback(Core::Result<QHash<Snowflake, Message>>::makeError(
                    "Failed to fetch forum post data: " + response.error));
            return;
        }

        // { "threads": { "<threadId>": { "first_message": message|null, "owner": … } } }
        QHash<Snowflake, Message> firstMessages;
        QJsonObject threads = QJsonDocument::fromJson(response.body).object().value("threads").toObject();
        for (auto it = threads.constBegin(); it != threads.constEnd(); ++it) {
            QJsonValue fm = it.value().toObject().value("first_message");
            if (fm.isObject())
                firstMessages.insert(Snowflake(it.key().toULongLong()),
                                     Message::fromJson(fm.toObject()));
        }
        callback(Core::Result<QHash<Snowflake, Message>>::makeOk(firstMessages));
    });
}

void Client::onConnected()
{
    qInfo() << "Connected to gateway";

    setState(Core::ConnectionState::Connected);
}

void Client::onDisconnected(CloseCode code, const QString &reason)
{
    qWarning() << "Disconnected from gateway: " << code << reason;

    // Fatal close codes — no reconnection, transition straight to Disconnected
    if (code == CloseCode::AUTHENTICATION_FAILED ||
        code == CloseCode::INVALID_SHARD ||
        code == CloseCode::SHARDING_REQUIRED ||
        code == CloseCode::INVALID_API_VERSION ||
        code == CloseCode::INVALID_INTENTS ||
        code == CloseCode::DISALLOWED_INTENTS) {
        setState(Core::ConnectionState::Disconnected);
        if (code == CloseCode::AUTHENTICATION_FAILED) {
            emit errorOccurred("Invalid token");
            emit authenticationFailed();
        } else {
            emit errorOccurred("Fatal gateway error: " + reason);
        }
        return;
    }

    // User-initiated disconnect (via stop()) — transition to Disconnected
    if (state == Core::ConnectionState::Disconnecting) {
        setState(Core::ConnectionState::Disconnected);
        return;
    }

    // Non-fatal: Gateway will handle reconnection automatically
    // stateChanged(Connecting) is emitted via the reconnecting signal
}

void Client::onGatewayReady(const Ready &data)
{
    for (const auto &guild : data.guilds.get())
        indexGuildMappings(guild);

    settings = Proto::PreloadedUserSettings::fromBase64(data.userSettingsProto.get());

    if ((!settings.guildFolders.has_value() || settings.guildFolders->folders.isEmpty()) && data.userSettings.hasValue() && !data.userSettings->guildFolders->isEmpty())
        settings.guildFolders = guildFoldersFromLegacy(data.userSettings->guildFolders.get());

    applyDiscordLocale();

    me = data.user;

    emit ready(data);
}

void Client::onGatewayReadySupplemental(const ReadySupplemental &data)
{
    emit readySupplemental(data);
}

void Client::onGatewayUserSettingsProtoUpdate(const UserSettingsProtoUpdate &event)
{
    if (event.type.get() == UserSettingsProtoType::PRELOADED) {
        auto updated = Proto::PreloadedUserSettings::fromBase64(event.proto.get());

        if (event.partial.hasValue() && event.partial.get()) {
            settings.mergeFrom(updated);
        } else {
            if (!updated.guildFolders.has_value() || updated.guildFolders->folders.isEmpty())
                updated.guildFolders = settings.guildFolders;
            settings = updated;
        }

        applyDiscordLocale();
        emit settingsChanged();
    }

    emit userSettingsProtoUpdated(event);
}

void Client::applyDiscordLocale()
{
    if (settings.localization.has_value() && settings.localization->locale.has_value())
        identity.setDiscordLocale(settings.localization->locale.value());
}

void Client::onGatewayMessageCreate(const Message &msg)
{
    emit messageCreated(msg);
}

void Client::onGatewayMessageUpdate(const Message &msg)
{
    emit messageUpdated(msg);
}

void Client::onGatewayMessageDelete(const MessageDelete &event)
{
    emit messageDeleted(event);
}

void Client::onGatewayChannelCreate(const ChannelCreate &event)
{
    emit channelCreated(event);
}

void Client::onGatewayGuildCreate(const GatewayGuild &guild)
{
    indexGuildMappings(guild);

    emit guildCreated(guild);
}

void Client::onGatewayGuildUpdate(const Guild &guild)
{
    guildPremiumTiers.insert(guild.id.get(), guild.premiumTier.valueOr(PremiumTier::NONE));

    emit guildUpdated(guild);
}

void Client::onGatewayGuildDelete(const GuildDelete &event)
{
    if (event.userRemoved() && event.id.hasValue())
        removeGuildMappings(event.id.get());

    emit guildDeleted(event);
}

void Client::indexGuildMappings(const GatewayGuild &guild)
{
    if (!guild.properties.hasValue())
        return;

    Snowflake guildId = guild.properties->id.get();
    if (guild.channels.hasValue())
        for (const auto &channel : guild.channels.get())
            channelToGuild.insert(channel.id, guildId);
    if (guild.threads.hasValue())
        for (const auto &thread : guild.threads.get())
            channelToGuild.insert(thread.id, guildId);
    guildPremiumTiers.insert(guildId, guild.properties->premiumTier.hasValue()
                                              ? guild.properties->premiumTier.get()
                                              : PremiumTier::NONE);
}

void Client::removeGuildMappings(Snowflake guildId)
{
    for (auto it = channelToGuild.begin(); it != channelToGuild.end();) {
        if (it.value() == guildId)
            it = channelToGuild.erase(it);
        else
            ++it;
    }
    guildPremiumTiers.remove(guildId);
}

void Client::onGatewayChannelUpdate(const ChannelUpdate &event)
{
    emit channelUpdated(event);
}

void Client::onGatewayChannelDelete(const ChannelDelete &event)
{
    emit channelDeleted(event);
}

void Client::onGatewayThreadCreate(const ChannelCreate &event)
{
    const Channel &thread = event.channel.get();
    Snowflake guildId = thread.guildId.hasValue() ? thread.guildId.get() : Snowflake::Invalid;
    if (!guildId.isValid() && thread.parentId.hasValue()) {
        auto it = channelToGuild.constFind(thread.parentId.get());
        if (it != channelToGuild.constEnd())
            guildId = it.value();
    }
    if (guildId.isValid())
        channelToGuild.insert(thread.id, guildId);

    emit threadCreated(event);
}

void Client::onGatewayThreadUpdate(const ChannelUpdate &event)
{
    const Channel &thread = event.channel.get();
    if (thread.guildId.hasValue())
        channelToGuild.insert(thread.id, thread.guildId.get());

    emit threadUpdated(event);
}

void Client::onGatewayThreadDelete(const ThreadDelete &event)
{
    channelToGuild.remove(event.id);

    emit threadDeleted(event);
}

void Client::onGatewayThreadListSync(const ThreadListSync &event)
{
    Snowflake guildId = event.guildId.get();
    if (event.threads.hasValue())
        for (const auto &thread : event.threads.get())
            channelToGuild.insert(thread.id, guildId);

    emit threadListSync(event);
}

void Client::onGatewayGuildRoleCreate(const GuildRoleCreate &event)
{
    emit guildRoleCreated(event);
}

void Client::onGatewayGuildRoleUpdate(const GuildRoleUpdate &event)
{
    emit guildRoleUpdated(event);
}

void Client::onGatewayGuildRoleDelete(const GuildRoleDelete &event)
{
    emit guildRoleDeleted(event);
}

void Client::sendMessage(Snowflake channelId, const QString &content, const QString &nonce,
                         Snowflake replyToMessageId, const QList<Core::PendingAttachment> &attachments)
{
    // todo extract to struct probably
    QJsonObject payload;
    payload["content"] = content;
    payload["flags"] = 0;
    payload["mobile_network_type"] = "unknown";
    payload["nonce"] = nonce;
    payload["tts"] = false;

    if (replyToMessageId.isValid()) {
        QJsonObject messageReference;
        messageReference["message_id"] = QString::number(replyToMessageId);
        messageReference["channel_id"] = QString::number(channelId);
        payload["message_reference"] = messageReference;
    }

    if (!attachments.isEmpty()) {
        auto state = std::make_shared<UploadState>();
        state->channelId = channelId;
        state->nonce = nonce;
        state->attachments = attachments;
        state->onUploaded = [this, channelId, nonce, payload](const QJsonArray &attachmentsJson) {
            QJsonObject withFiles = payload;
            withFiles["attachments"] = attachmentsJson;

            QString endpoint = "/channels/" + QString::number(channelId) + "/messages";
            httpClient->post(endpoint, withFiles, ContextProperties::location("chat_input"),
                             [this, nonce](const HttpResponse &response) {
                                 if (!response.success) {
                                     qCWarning(LogDiscord) << "Failed to send message:" << response.error
                                                           << "Status:" << response.statusCode;
                                     emit messageSendFailed(nonce, response.error);
                                 }
                             });
        };
        state->onFailed = [this, nonce](const QString &error) {
            emit messageSendFailed(nonce, error);
        };
        for (int i = 0; i < attachments.size(); i++) {
            state->uploadFilenames.append(QString());
            state->uploaded.append(false);
        }
        state->cancelFlag = std::make_shared<std::atomic<bool>>(false);
        activeUploads.insert(nonce, state);
        uploadAttachmentsAndSend(state);
        return;
    }

    QString endpoint = "/channels/" + QString::number(channelId) + "/messages";
    httpClient->post(endpoint, payload, ContextProperties::location("chat_input"),
                     [this, channelId, nonce](const HttpResponse &response) {
                         if (!response.success) {
                             qCWarning(LogDiscord) << "Failed to send message:" << response.error
                                                   << "Status:" << response.statusCode;
                             emit messageSendFailed(nonce, response.error);
                             return;
                         }

                         qCInfo(LogDiscord) << "Message sent successfully to channel" << channelId;
                     });
}

void Client::uploadAttachmentsAndSend(const std::shared_ptr<UploadState> &state)
{
    QJsonArray files;
    for (int i = 0; i < state->attachments.size(); i++) {
        QJsonObject file;
        file["id"] = QString::number(i);
        file["filename"] = state->attachments[i].filename;
        file["file_size"] = state->attachments[i].size;
        files.append(file);
    }
    QJsonObject body;
    body["files"] = files;

    QString endpoint = "/channels/" + QString::number(state->channelId) + "/attachments";
    httpClient->post(endpoint, body, [this, state](const HttpResponse &response) {
        if (state->cancelFlag->load()) {
            settleUpload(state);
            return;
        }
        if (!response.success) {
            qCWarning(LogDiscord) << "Failed to request upload slots:" << response.error
                                  << "Status:" << response.statusCode;
            settleUpload(state);
            failUpload(state, response.error);
            return;
        }

        const auto uploadSlots = QJsonDocument::fromJson(response.body).object()["attachments"].toArray();
        if (uploadSlots.size() != state->attachments.size()) {
            settleUpload(state);
            failUpload(state, "Unexpected upload slot response");
            return;
        }

        QStringList uploadUrls;
        for (int i = 0; i < state->attachments.size(); i++)
            uploadUrls.append(QString());
        for (const QJsonValue &slotValue : uploadSlots) {
            auto slot = slotValue.toObject();
            int index = slot["id"].toVariant().toInt();
            if (index < 0 || index >= state->attachments.size()) {
                // wut
                settleUpload(state);
                failUpload(state, "Unexpected upload slot response");
                return;
            }
            state->uploadFilenames[index] = slot["upload_filename"].toString();
            uploadUrls[index] = slot["upload_url"].toString();
        }

        state->remaining = state->attachments.size();
        for (int index = 0; index < state->attachments.size(); index++) {
            const auto &attachment = state->attachments[index];

            auto onDone = [this, state, index](const HttpResponse &putResponse) {
                state->remaining--;
                if (putResponse.success) {
                    state->uploaded[index] = true;
                } else if (!state->failed && !state->cancelFlag->load()) {
                    state->failed = true;
                    qCWarning(LogDiscord) << "Attachment upload failed:" << putResponse.error
                                          << "Status:" << putResponse.statusCode;
                    failUpload(state, putResponse.error);
                    state->cancelFlag->store(true); // abort !!!
                }
                if (state->remaining > 0)
                    return;

                if (state->failed || state->cancelFlag->load()) {
                    cleanupUploadedSlots(state);
                    settleUpload(state);
                    return;
                }
                finishUpload(state);
            };
            auto onProgress = [this, state, index](qint64 sent, qint64 total) {
                emit attachmentUploadProgress(state->nonce, index, sent, total);
            };

            // pasted bitmap from mem, otherwise from disk
            if (!attachment.data.isEmpty())
                httpClient->putExternal(uploadUrls[index], attachment.data, attachment.mimeType,
                                        onDone, onProgress, state->cancelFlag);
            else
                httpClient->putExternalFile(uploadUrls[index], attachment.filePath,
                                            attachment.mimeType, onDone, onProgress,
                                            state->cancelFlag);
        }
    });
}

void Client::finishUpload(const std::shared_ptr<UploadState> &state)
{
    if (state->cancelFlag->load()) {
        cleanupUploadedSlots(state);
        settleUpload(state);
        return;
    }

    QJsonArray attachmentsJson;
    for (int i = 0; i < state->attachments.size(); i++) {
        const auto &attachment = state->attachments[i];
        QJsonObject obj;
        obj["id"] = QString::number(i);
        obj["filename"] = attachment.filename;
        obj["uploaded_filename"] = state->uploadFilenames[i];
        if (attachment.isSpoiler)
            obj["is_spoiler"] = true;
        if (!attachment.description.isEmpty())
            obj["description"] = attachment.description;
        attachmentsJson.append(obj);
    }

    auto onUploaded = state->onUploaded;
    settleUpload(state);
    onUploaded(attachmentsJson);
}

void Client::failUpload(const std::shared_ptr<UploadState> &state, const QString &error)
{
    if (state->onFailed)
        state->onFailed(error);
}

void Client::cleanupUploadedSlots(const std::shared_ptr<UploadState> &state)
{
    for (int i = 0; i < state->uploaded.size(); i++) {
        if (!state->uploaded[i] || state->uploadFilenames[i].isEmpty())
            continue;
        httpClient->delete_("/attachments/" + state->uploadFilenames[i], [](const auto &) {});
    }
}

void Client::settleUpload(const std::shared_ptr<UploadState> &state)
{
    activeUploads.remove(state->nonce);
}

bool Client::cancelMessageSend(const QString &nonce)
{
    auto it = activeUploads.constFind(nonce);
    if (it == activeUploads.constEnd())
        return false;
    it.value()->cancelFlag->store(true);
    return true;
}

void Client::editMessage(Snowflake channelId, Snowflake messageId, const QString &content)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/messages/" +
                       QString::number(messageId);

    QJsonObject payload;
    payload["content"] = content;

    httpClient->patch(endpoint, payload, [this, channelId, messageId](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to edit message" << messageId << "in channel"
                                  << channelId << ":" << response.error;
        else
            qCInfo(LogDiscord) << "Message" << messageId << "edited in channel" << channelId;
    });
}

void Client::deleteMessage(Snowflake channelId, Snowflake messageId)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/messages/" +
                       QString::number(messageId);

    httpClient->delete_(endpoint, [this, channelId, messageId](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to delete message" << messageId << "in channel"
                                  << channelId << ":" << response.error;
        else
            qCInfo(LogDiscord) << "Message" << messageId << "deleted from channel" << channelId;
    });
}

void Client::pinMessage(Snowflake channelId, Snowflake messageId)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/pins/" +
                       QString::number(messageId);

    httpClient->put(endpoint, QJsonObject{}, [this, channelId, messageId](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to pin message" << messageId << "in channel"
                                  << channelId << ":" << response.error;
        else
            qCInfo(LogDiscord) << "Message" << messageId << "pinned in channel" << channelId;
    });
}

void Client::unpinMessage(Snowflake channelId, Snowflake messageId)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/pins/" +
                       QString::number(messageId);

    httpClient->delete_(endpoint, [this, channelId, messageId](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to unpin message" << messageId << "in channel"
                                  << channelId << ":" << response.error;
        else
            qCInfo(LogDiscord) << "Message" << messageId << "unpinned from channel" << channelId;
    });
}

namespace {

constexpr int ReactionTypeNormal = 0;
constexpr int ReactionTypeBurst = 1;

QString reactionLocationName(Client::ReactionLocation location)
{
    switch (location) {
    case Client::ReactionLocation::HoverBar:
        return QStringLiteral("Message Hover Bar");
    case Client::ReactionLocation::InlineButton:
        return QStringLiteral("Message Inline Button");
    case Client::ReactionLocation::ContextMenu:
        return QStringLiteral("Message Context Menu");
    case Client::ReactionLocation::ReactionPicker:
        return QStringLiteral("Message Reaction Picker");
    case Client::ReactionLocation::Message:
        return QStringLiteral("Message");
    }
    return {};
}

QString reactionsEndpoint(Core::Snowflake channelId, Core::Snowflake messageId, const QString &reactionKey)
{
    return "/channels/" + QString::number(channelId) + "/messages/" + QString::number(messageId) + "/reactions/" + QString::fromLatin1(QUrl::toPercentEncoding(reactionKey, ":"));
}

QString reactionTypeParameter(bool isBurst)
{
    return QString::number(isBurst ? ReactionTypeBurst : ReactionTypeNormal);
}

Client::ReactionResult reactionResult(const HttpResponse &response)
{
    Client::ReactionResult result;
    result.success = response.success;
    result.statusCode = response.statusCode;
    if (response.success)
        return result;

    const QJsonObject body = QJsonDocument::fromJson(response.body).object();
    result.errorCode = body.value("code").toInt();
    if (response.rateLimited())
        result.retryAfterSeconds = qMax(1, qCeil(body.value("retry_after").toDouble(1.0)));
    return result;
}

} // namespace

void Client::addReaction(Snowflake channelId, Snowflake messageId, const QString &reactionKey, bool isBurst, ReactionLocation location, ReactionCallback callback)
{
    QUrlQuery query;
    query.addQueryItem("location", reactionLocationName(location));
    query.addQueryItem("type", reactionTypeParameter(isBurst));
    const QString endpoint = reactionsEndpoint(channelId, messageId, reactionKey) + "/%40me?" + query.toString(QUrl::FullyEncoded);

    httpClient->put(endpoint, QJsonObject{}, [channelId, messageId, callback](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to add reaction on message" << messageId << "in channel" << channelId << ":" << response.error;
        if (callback)
            callback(reactionResult(response));
    });
}

void Client::removeReaction(Snowflake channelId, Snowflake messageId, const QString &reactionKey, bool isBurst, ReactionLocation location, std::optional<Snowflake> reactorUnlessMe, ReactionCallback callback)
{
    QUrlQuery query;
    query.addQueryItem("location", reactionLocationName(location));
    query.addQueryItem("burst", isBurst ? "true" : "false");
    const QString reactor = reactorUnlessMe ? QString::number(*reactorUnlessMe) : QStringLiteral("%40me");
    const QString endpoint = reactionsEndpoint(channelId, messageId, reactionKey) + "/" + reactionTypeParameter(isBurst) + "/" + reactor + "?" + query.toString(QUrl::FullyEncoded);

    httpClient->delete_(endpoint, [channelId, messageId, callback](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to remove reaction on message" << messageId << "in channel" << channelId << ":" << response.error;
        if (callback)
            callback(reactionResult(response));
    });
}

void Client::fetchReactors(Snowflake channelId, Snowflake messageId, const QString &reactionKey, bool isBurst, int limit, std::optional<Snowflake> after, ResultCallback<QList<User>> callback)
{
    QUrlQuery query;
    query.addQueryItem("limit", QString::number(limit));
    if (after)
        query.addQueryItem("after", QString::number(*after));
    query.addQueryItem("type", reactionTypeParameter(isBurst));
    httpClient->get(reactionsEndpoint(channelId, messageId, reactionKey), query, valueHandler<QList<User>>("Fetching reactors", std::move(callback), parseArray<User>));
}

void Client::leaveGuild(Snowflake guildId)
{
    QString endpoint = "/users/@me/guilds/" + QString::number(guildId);
    httpClient->delete_(endpoint, QJsonObject{}, [this, guildId](const HttpResponse &response) {
        if (!response.success) {
            QString err = QStringLiteral("status=%1 error=%2")
                                  .arg(response.statusCode)
                                  .arg(response.error);
            qCWarning(LogDiscord) << "Failed to leave guild" << guildId << err;
            emit guildLeaveFailed(guildId, err);
        }
    });
}

void Client::debugForceReconnect()
{
    gateway->debugForceReconnect();
}

void Client::setVoiceConnected(bool connected)
{
    if (voiceConnected == connected)
        return;
    voiceConnected = connected;
    updateActiveState();
}

void Client::updateActiveState()
{
    identity.setActivity(appFocused, voiceConnected);
    gateway->setActiveState(appFocused, voiceConnected);
    syncHeartbeatSession();

    if (appFocused || voiceConnected) {
        if (!heartbeatSessionTimer->isActive())
            heartbeatSessionTimer->start();
    } else {
        heartbeatSessionTimer->stop();
    }
}

void Client::restoreHeartbeatSession(const std::optional<HeartbeatSession> &stored)
{
    identity.restoreHeartbeatSession(stored);
    syncHeartbeatSession();
}

void Client::onHeartbeatSessionTimer()
{
    syncHeartbeatSession();
}

void Client::syncHeartbeatSession()
{
    HeartbeatSessionUpdate update = identity.touchHeartbeatSession();
    if (update == HeartbeatSessionUpdate::Unchanged)
        return;

    if (std::optional<HeartbeatSession> session = identity.heartbeatSession())
        emit heartbeatSessionChanged(*session);

    if (update == HeartbeatSessionUpdate::Created)
        gateway->sendUpdateTimeSpentSessionId();
}

void Client::ackMessage(Snowflake channelId, Snowflake messageId, int flags, int lastViewed)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/messages/" +
                       QString::number(messageId) + "/ack";

    QJsonObject payload;
    payload["flags"] = flags;
    payload["last_viewed"] = lastViewed;
    payload["token"] = QJsonValue::Null;

    httpClient->post(endpoint, payload, [this, channelId](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to ack message in channel" << channelId
                                  << ":" << response.error;
    });
}

void Client::ackBulk(const QList<AckEntry> &entries, std::function<void(bool success)> onFinished)
{
    QJsonArray readStates;
    for (const auto &entry : entries) {
        QJsonObject obj;
        obj["channel_id"] = QString::number(entry.channelId);
        obj["message_id"] = QString::number(entry.messageId);
        obj["read_state_type"] = entry.readStateType;
        readStates.append(obj);
    }

    QJsonObject payload;
    payload["read_states"] = readStates;
    httpClient->post("/read-states/ack-bulk", payload, [onFinished = std::move(onFinished)](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to bulk ack:" << response.error;
        onFinished(response.success);
    });
}

void Client::subscribeToGuildChannel(Snowflake guildId, Snowflake channelId,
                                     const QList<QPair<int, int>> &ranges)
{
    gateway->subscribeToGuild(guildId, channelId, ranges);
    subscribedGuilds.insert(guildId);
}

void Client::ensureSubscriptionByChannel(Snowflake channelId)
{
    if (!channelToGuild.contains(channelId))
        return;

    Snowflake guildId = channelToGuild.value(channelId);
    if (!subscribedGuilds.contains(guildId)) {
        QList<QPair<int, int>> defaultRanges = { { 0, 99 } };
        subscribeToGuildChannel(guildId, channelId, defaultRanges);
    }
}

void Client::requestForumUnreads(Snowflake forumId, const QList<QPair<Snowflake, Snowflake>> &threads)
{
    if (threads.isEmpty())
        return;

    Snowflake guildId = getGuildIdForChannel(forumId);
    if (!guildId.isValid())
        return;

    gateway->requestForumUnreads(guildId, forumId, threads);
}

Snowflake Client::getGuildIdForChannel(Snowflake channelId) const
{
    return channelToGuild.value(channelId, Snowflake::Invalid);
}

void Client::registerChannelGuild(Snowflake channelId, Snowflake guildId)
{
    if (channelId.isValid() && guildId.isValid())
        channelToGuild.insert(channelId, guildId);
}

PremiumTier Client::getGuildPremiumTier(Snowflake guildId) const
{
    return guildPremiumTiers.value(guildId, PremiumTier::NONE);
}

qint64 Client::getMaxUploadSize(Snowflake channelId) const
{
    constexpr qint64 MiB = 1024 * 1024;

    auto premiumType = me.premiumType.hasValue() ? me.premiumType.get() : PremiumType::NONE;
    qint64 userLimit = 10 * MiB;
    switch (premiumType) {
    case PremiumType::TIER_1:
        userLimit = 10 * MiB;
        break;
    case PremiumType::TIER_2:
        userLimit = 500 * MiB;
        break;
    case PremiumType::TIER_3:
        userLimit = 50 * MiB;
        break;
    default:
        break;
    }

    qint64 guildLimit = 10 * MiB;
    Snowflake guildId = getGuildIdForChannel(channelId);
    if (guildId.isValid()) {
        switch (getGuildPremiumTier(guildId)) {
        case PremiumTier::TIER_2:
            guildLimit = 50 * MiB;
            break;
        case PremiumTier::TIER_3:
            guildLimit = 100 * MiB;
            break;
        default:
            break;
        }
    }

    return qMax(userLimit, guildLimit);
}

void Client::sendVoiceStateUpdate(Snowflake guildId, Snowflake channelId, bool selfMute, bool selfDeaf)
{
    gateway->sendVoiceStateUpdate(guildId, channelId, selfMute, selfDeaf);
}

void Client::requestGuildMembers(Snowflake guildId, const QList<Snowflake> &userIds, bool presences)
{
    gateway->requestGuildMembers(guildId, userIds, presences);
}

void Client::queryGuildMembers(Snowflake guildId, const QString &query, int limit)
{
    gateway->queryGuildMembers(guildId, query, limit, false);
}

[[nodiscard]] const Proto::PreloadedUserSettings &Client::getSettings() const
{
    return settings;
}

[[nodiscard]] const User &Client::getMe() const
{
    return me;
}

bool Client::isPremium() const
{
    return me.premiumType.hasValue() && me.premiumType.get() != PremiumType::NONE;
}

void Client::fetchFrecencySettings(FrecencyCallback callback)
{
    QPointer<Client> self(this);
    httpClient->get(frecencySettingsEndpoint(), {}, [self, callback](const HttpResponse &response) {
        if (!response.success) {
            QString err = QStringLiteral("status=%1 error=%2")
                                  .arg(response.statusCode)
                                  .arg(response.error);
            qCWarning(LogDiscord) << "Failed to fetch frecency settings:" << err;
            callback(Core::Result<Proto::FrecencyUserSettings>::makeError(err));
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(response.body).object();
        if (!obj.value("settings").isString()) {
            qCWarning(LogDiscord) << "Frecency settings response carries no settings";
            callback(Core::Result<Proto::FrecencyUserSettings>::makeError(QStringLiteral("response carries no settings")));
            return;
        }

        const Proto::FrecencyUserSettings settings = decodeFrecencySettings(obj).value_or(Proto::FrecencyUserSettings());
        if (self)
            emit self->frecencySettingsReceived(settings);
        callback(Core::Result<Proto::FrecencyUserSettings>::makeOk(settings));
    });
}

void Client::patchFrecencySettings(const QByteArray &partialProto,
                                   std::optional<uint32_t> requiredDataVersion,
                                   FrecencyPatchCallback callback)
{
    QJsonObject payload;
    payload["settings"] = QString::fromLatin1(partialProto.toBase64());
    if (requiredDataVersion)
        payload["required_data_version"] = static_cast<qint64>(*requiredDataVersion);

    QPointer<Client> self(this);
    httpClient->patch(frecencySettingsEndpoint(), payload,
                      [self, callback](const HttpResponse &response) {
                          FrecencyPatchResult result;
                          result.rateLimited = response.rateLimited();

                          if (!response.success) {
                              result.error = QStringLiteral("status=%1 error=%2")
                                                     .arg(response.statusCode)
                                                     .arg(response.error);
                              qCWarning(LogDiscord) << "Failed to patch frecency settings:" << result.error;
                              callback(result);
                              return;
                          }

                          const QJsonObject obj = QJsonDocument::fromJson(response.body).object();
                          result.success = true;
                          result.outOfDate = obj.value("out_of_date").toBool();
                          result.settings = decodeFrecencySettings(obj);
                          if (self && result.settings)
                              emit self->frecencySettingsReceived(*result.settings);

                          callback(result);
                      });
}

HttpCallback Client::applyThen(std::function<void(const QJsonDocument &)> apply, HttpCallback finish)
{
    QPointer<Client> self(this);
    return [self, apply = std::move(apply), finish = std::move(finish)](const HttpResponse &response) {
        if (self && response.success)
            apply(QJsonDocument::fromJson(response.body));
        finish(response);
    };
}

void Client::fetchGuildProfile(Snowflake guildId, ResultCallback<GuildProfileEdit> callback)
{
    httpClient->get(guildEndpoint(guildId, "/profile"), {},
                    valueHandler<GuildProfileEdit>("Fetching the guild profile", std::move(callback),
                                                   [](const QJsonDocument &doc) {
                                                       return GuildProfileEdit::fromProfile(doc.object());
                                                   }));
}

void Client::modifyGuildProfile(Snowflake guildId, const GuildProfileEdit &edit, ResultCallback<GuildProfileEdit> callback)
{
    httpClient->patch(guildEndpoint(guildId, "/profile"), edit.toJson(),
                      valueHandler<GuildProfileEdit>("Saving the guild profile", std::move(callback),
                                                     [](const QJsonDocument &doc) {
                                                         return GuildProfileEdit::fromProfile(doc.object());
                                                     }));
}

void Client::modifyGuild(Snowflake guildId, const GuildEdit &edit, const QMap<QString, QByteArray> &originalMd5s, ActionCallback callback)
{
    RequestOptions options;
    options.originalMd5 = originalMd5Header(originalMd5s);
    httpClient->send(HttpClient::Method::PATCH, guildEndpoint(guildId), edit.toJson().toBytes(), options,
                     applyThen([this](const QJsonDocument &doc) { onGatewayGuildUpdate(Guild::fromJson(doc.object())); },
                               actionHandler("Saving guild settings", std::move(callback))));
}

void Client::deleteGuild(Snowflake guildId, ActionCallback callback)
{
    httpClient->send(HttpClient::Method::POST, guildEndpoint(guildId, "/delete"), {}, {}, actionHandler("Deleting a guild", std::move(callback)));
}

void Client::createRole(Snowflake guildId, ResultCallback<Role> callback)
{
    Core::OrderedJson colors;
    colors.insert("primary_color", 0);
    colors.insert("secondary_color", QJsonValue::Null);
    colors.insert("tertiary_color", QJsonValue::Null);

    Core::OrderedJson body;
    body.insert("name", "new role");
    body.insert("color", 0);
    body.insert("colors", colors);
    body.insert("permissions", "0");

    httpClient->post(guildEndpoint(guildId, "/roles"), body,
                     applyThen(
                             [this, guildId](const QJsonDocument &doc) {
                                 GuildRoleCreate event;
                                 event.guildId = guildId;
                                 event.role = Role::fromJson(doc.object());
                                 onGatewayGuildRoleCreate(event);
                             },
                             valueHandler<Role>("Creating a role", std::move(callback),
                                                [](const QJsonDocument &doc) { return Role::fromJson(doc.object()); })));
}

void Client::modifyRole(Snowflake guildId, Snowflake roleId, const RoleEdit &edit, ActionCallback callback)
{
    httpClient->patch(roleEndpoint(guildId, roleId), edit.toJson(),
                      applyThen(
                              [this, guildId](const QJsonDocument &doc) {
                                  GuildRoleUpdate event;
                                  event.guildId = guildId;
                                  event.role = Role::fromJson(doc.object());
                                  onGatewayGuildRoleUpdate(event);
                              },
                              actionHandler("Saving a role", std::move(callback))));
}

void Client::modifyRolePositions(Snowflake guildId, const QList<QPair<Snowflake, int>> &positions, ActionCallback callback)
{
    Core::OrderedJson::Array body;
    QSet<Snowflake> moved;
    for (const auto &[roleId, position] : positions) {
        Core::OrderedJson entry;
        entry.insert("id", QString::number(roleId));
        entry.insert("position", position);
        body.append(entry);
        moved.insert(roleId);
    }

    httpClient->patch(guildEndpoint(guildId, "/roles"), body,
                      applyThen(
                              [this, guildId, moved](const QJsonDocument &doc) {
                                  for (const Role &role : parseArray<Role>(doc)) {
                                      if (!moved.contains(role.id.get()))
                                          continue;
                                      GuildRoleUpdate event;
                                      event.guildId = guildId;
                                      event.role = role;
                                      onGatewayGuildRoleUpdate(event);
                                  }
                              },
                              actionHandler("Reordering roles", std::move(callback))));
}

void Client::deleteRole(Snowflake guildId, Snowflake roleId, ActionCallback callback)
{
    httpClient->delete_(roleEndpoint(guildId, roleId), applyThen(
                                                               [this, guildId, roleId](const QJsonDocument &) {
                                                                   GuildRoleDelete event;
                                                                   event.guildId = guildId;
                                                                   event.roleId = roleId;
                                                                   onGatewayGuildRoleDelete(event);
                                                               },
                                                               actionHandler("Deleting a role", std::move(callback))));
}

void Client::fetchRoleMemberCounts(Snowflake guildId, ResultCallback<QHash<Snowflake, int>> callback)
{
    httpClient->get(guildEndpoint(guildId, "/roles/member-counts"), {},
                    valueHandler<QHash<Snowflake, int>>("Fetching role member counts", std::move(callback),
                                                        [](const QJsonDocument &doc) {
                                                            QHash<Snowflake, int> counts;
                                                            const QJsonObject obj = doc.object();
                                                            for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
                                                                counts.insert(Snowflake(it.key().toULongLong()), it.value().toInt());
                                                            return counts;
                                                        }));
}

void Client::fetchRoleMemberIds(Snowflake guildId, Snowflake roleId, ResultCallback<QList<Snowflake>> callback)
{
    httpClient->get(roleEndpoint(guildId, roleId, "/member-ids"), {},
                    valueHandler<QList<Snowflake>>("Fetching role members", std::move(callback),
                                                   [](const QJsonDocument &doc) {
                                                       QList<Snowflake> ids;
                                                       for (const QJsonValue &value : doc.array())
                                                           ids.append(Snowflake(value.toString().toULongLong()));
                                                       return ids;
                                                   }));
}

void Client::addRoleMembers(Snowflake guildId, Snowflake roleId, const QList<Snowflake> &userIds, ResultCallback<QList<Snowflake>> callback)
{
    Core::OrderedJson body;
    body.insert("member_ids", snowflakeArray(userIds));

    QPointer<Client> self(this);
    auto counted = [self, guildId, roleId, callback = std::move(callback)](const Core::Result<QList<Snowflake>> &result) {
        if (self && result.success())
            emit self->roleMemberCountChanged(guildId, roleId, int(result.value->size()));
        callback(result);
    };
    httpClient->patch(roleEndpoint(guildId, roleId, "/members"), body,
                      applyThen(
                              [this, guildId](const QJsonDocument &doc) {
                                  const QJsonObject members = doc.object();
                                  for (auto it = members.constBegin(); it != members.constEnd(); ++it) {
                                      GuildMemberUpdate event;
                                      event.guildId = guildId;
                                      event.member = Member::fromJson(it.value().toObject());
                                      emit guildMemberUpdated(event);
                                  }
                              },
                              valueHandler<QList<Snowflake>>("Adding role members", std::move(counted), [](const QJsonDocument &doc) {
                                  QList<Snowflake> added;
                                  const QJsonObject members = doc.object();
                                  for (auto it = members.constBegin(); it != members.constEnd(); ++it)
                                      added.append(Snowflake(it.key().toULongLong()));
                                  return added;
                              })));
}

void Client::searchGuildMembers(Snowflake guildId, const MemberSearchQuery &query, ResultCallback<MemberSearchPage> callback)
{
    httpClient->post(guildEndpoint(guildId, "/members-search"), query.toJson(), [callback](const HttpResponse &response) {
        if (!response.success) {
            const ApiError error = ApiError::fromResponse(response);
            qCWarning(LogDiscord) << "Member search failed with status" << response.statusCode << ":" << error.message;
            callback(Core::Result<MemberSearchPage>::makeError(error.message, error.code));
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(response.body).object();
        MemberSearchPage page;
        if (response.statusCode == 202) {
            page.indexingRetryAfterSeconds = qMax(1, qRound(obj.value("retry_after").toDouble(1.0)));
            callback(Core::Result<MemberSearchPage>::makeOk(page));
            return;
        }

        for (const QJsonValue &value : obj.value("members").toArray())
            page.members.append(MemberSearchResult::fromJson(value.toObject()));
        page.totalResultCount = obj.value("total_result_count").toInt();
        callback(Core::Result<MemberSearchPage>::makeOk(page));
    });
}

HttpCallback Client::memberUpdateHandler(Snowflake guildId, const char *what, ActionCallback callback)
{
    return applyThen(
            [this, guildId](const QJsonDocument &doc) {
                GuildMemberUpdate event;
                event.guildId = guildId;
                event.member = Member::fromJson(doc.object());
                emit guildMemberUpdated(event);
            },
            actionHandler(what, std::move(callback)));
}

HttpCallback Client::memberRemovalHandler(Snowflake guildId, Snowflake userId, const char *what, ActionCallback callback)
{
    return applyThen(
            [this, guildId, userId](const QJsonDocument &) {
                User user;
                user.id = userId;
                GuildMemberRemove event;
                event.guildId = guildId;
                event.user = user;
                emit guildMemberRemoved(event);
            },
            actionHandler(what, std::move(callback)));
}

void Client::setMemberRoles(Snowflake guildId, Snowflake userId, const QList<Snowflake> &roleIds,
                            const QList<Snowflake> &added, const QList<Snowflake> &removed, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("roles", snowflakeArray(roleIds));

    QPointer<Client> self(this);
    auto counted = [self, guildId, added, removed, callback = std::move(callback)](const Core::Result<void> &result) {
        if (self && result.success()) {
            for (Snowflake roleId : added)
                emit self->roleMemberCountChanged(guildId, roleId, 1);
            for (Snowflake roleId : removed)
                emit self->roleMemberCountChanged(guildId, roleId, -1);
        }
        if (callback)
            callback(result);
    };
    httpClient->patch(memberEndpoint(guildId, userId), body, memberUpdateHandler(guildId, "Updating member roles", std::move(counted)));
}

void Client::setMemberNickname(Snowflake guildId, Snowflake userId, const QString &nick, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("nick", nick);
    httpClient->patch(memberEndpoint(guildId, userId), body, memberUpdateHandler(guildId, "Changing a nickname", std::move(callback)));
}

void Client::setMemberTimeout(Snowflake guildId, Snowflake userId, const QDateTime &until, const std::optional<QString> &reason, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("communication_disabled_until", until.isValid() ? QJsonValue(isoTimestamp(until)) : QJsonValue());

    RequestOptions options;
    options.auditLogReason = reason;
    httpClient->send(HttpClient::Method::PATCH, memberEndpoint(guildId, userId), body.toBytes(), options,
                     memberUpdateHandler(guildId, "Timing out a member", std::move(callback)));
}

void Client::kickMember(Snowflake guildId, Snowflake userId, const QString &reason, ActionCallback callback)
{
    const QString endpoint = memberEndpoint(guildId, userId) + "?reason=" + QString::fromLatin1(encodeUriComponent(reason));
    httpClient->delete_(endpoint, memberRemovalHandler(guildId, userId, "Kicking a member", std::move(callback)));
}

void Client::banMember(Snowflake guildId, Snowflake userId, int deleteMessageSeconds, const QString &reason, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("delete_message_seconds", deleteMessageSeconds);

    RequestOptions options;
    options.auditLogReason = reason;
    httpClient->send(HttpClient::Method::PUT, guildEndpoint(guildId, "/bans/" + QString::number(userId)),
                     body.toBytes(), options,
                     memberRemovalHandler(guildId, userId, "Banning a member", std::move(callback)));
}

void Client::unbanMember(Snowflake guildId, Snowflake userId, ActionCallback callback)
{
    httpClient->delete_(guildEndpoint(guildId, "/bans/" + QString::number(userId)), actionHandler("Revoking a ban", std::move(callback)));
}

void Client::requestPruneCount(Snowflake guildId, int days, const QList<Snowflake> &includeRoles, ActionCallback callback)
{
    QUrlQuery query;
    query.addQueryItem("days", QString::number(days));
    for (Snowflake roleId : includeRoles)
        query.addQueryItem("include_roles", QString::number(roleId));
    httpClient->get(guildEndpoint(guildId, "/prune/v2"), query, actionHandler("Estimating a prune", std::move(callback)));
}

void Client::pruneMembers(Snowflake guildId, int days, const QList<Snowflake> &includeRoles, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("days", days);
    body.insert("compute_prune_count", false);
    body.insert("include_roles", snowflakeArray(includeRoles));
    httpClient->post(guildEndpoint(guildId, "/prune"), body, actionHandler("Pruning members", std::move(callback)));
}

void Client::setMemberUpdatesSubscription(Snowflake guildId, bool subscribed)
{
    gateway->setMemberUpdatesSubscription(guildId, subscribed);
}

void Client::fetchBans(Snowflake guildId, Snowflake after, ResultCallback<QList<Ban>> callback)
{
    QUrlQuery query;
    query.addQueryItem("limit", "1000");
    if (after.isValid())
        query.addQueryItem("after", QString::number(after));
    httpClient->get(guildEndpoint(guildId, "/bans"), query, valueHandler<QList<Ban>>("Fetching bans", std::move(callback), parseArray<Ban>));
}

void Client::searchBans(Snowflake guildId, const QString &text, const QList<Snowflake> &userIds, ResultCallback<QList<Ban>> callback)
{
    QUrlQuery query;
    query.addQueryItem("limit", "10");
    for (Snowflake userId : userIds)
        query.addQueryItem("user_ids", QString::number(userId));
    if (!text.trimmed().isEmpty())
        query.addQueryItem("query", text);
    httpClient->get(guildEndpoint(guildId, "/bans/search"), query, valueHandler<QList<Ban>>("Searching bans", std::move(callback), parseArray<Ban>));
}

void Client::fetchGuildInvites(Snowflake guildId, ResultCallback<QList<Invite>> callback)
{
    httpClient->get(guildEndpoint(guildId, "/invites"), {}, valueHandler<QList<Invite>>("Fetching invites", std::move(callback), parseArray<Invite>));
}

void Client::createInvite(Snowflake channelId, int maxAgeSeconds, int maxUses, bool temporary, ResultCallback<Invite> callback)
{
    Core::OrderedJson body;
    body.insert("max_age", maxAgeSeconds);
    body.insert("max_uses", maxUses);
    body.insert("temporary", temporary);
    body.insert("flags", 0);

    RequestOptions options;
    options.context = ContextProperties::location("Settings Invite");
    QPointer<Client> self(this);
    auto announced = [self, callback = std::move(callback)](const Core::Result<Invite> &result) {
        if (self && result.success())
            emit self->inviteCreated(*result.value);
        if (callback)
            callback(result);
    };
    httpClient->send(HttpClient::Method::POST, "/channels/" + QString::number(channelId) + "/invites",
                     body.toBytes(), options,
                     valueHandler<Invite>("Creating an invite", std::move(announced),
                                          [](const QJsonDocument &doc) { return Invite::fromJson(doc.object()); }));
}

void Client::revokeInvite(const QString &code, ActionCallback callback)
{
    QPointer<Client> self(this);
    auto announced = [self, code, callback = std::move(callback)](const Core::Result<void> &result) {
        if (self && (result.success() || result.code == ApiError::UnknownInvite))
            emit self->inviteRevoked(code);
        if (callback)
            callback(result);
    };
    httpClient->delete_("/invites/" + QString::fromLatin1(QUrl::toPercentEncoding(code)), actionHandler("Revoking an invite", std::move(announced)));
}

void Client::setIncidentActions(Snowflake guildId, const QDateTime &invitesDisabledUntil,
                                const QDateTime &dmsDisabledUntil, std::optional<int> lockdownHours,
                                ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("invites_disabled_until", invitesDisabledUntil.isValid() ? QJsonValue(isoTimestamp(invitesDisabledUntil)) : QJsonValue());
    body.insert("dms_disabled_until", dmsDisabledUntil.isValid() ? QJsonValue(isoTimestamp(dmsDisabledUntil)) : QJsonValue());
    body.insert("lockdown_duration_hours", lockdownHours ? QJsonValue(*lockdownHours) : QJsonValue());
    httpClient->put(guildEndpoint(guildId, "/incident-actions"), body, actionHandler("Updating invite and DM pauses", std::move(callback)));
}

void Client::fetchGuildEmojis(Snowflake guildId, ResultCallback<QList<Emoji>> callback)
{
    httpClient->get(guildEndpoint(guildId, "/emojis"), {}, valueHandler<QList<Emoji>>("Fetching emojis", std::move(callback), parseArray<Emoji>));
}

void Client::createGuildEmoji(Snowflake guildId, const QString &name, const QString &imageDataUri,
                              const QByteArray &originalMd5, ResultCallback<Emoji> callback)
{
    Core::OrderedJson body;
    body.insert("image", imageDataUri);
    body.insert("name", name);

    RequestOptions options;
    options.context = ContextProperties().add("client_event_source", "Guild Settings");
    options.originalMd5 = originalMd5;
    httpClient->send(HttpClient::Method::POST, guildEndpoint(guildId, "/emojis"), body.toBytes(), options,
                     valueHandler<Emoji>("Uploading an emoji", std::move(callback),
                                         [](const QJsonDocument &doc) { return Emoji::fromJson(doc.object()); }));
}

void Client::renameGuildEmoji(Snowflake guildId, Snowflake emojiId, const QString &name, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("name", name);
    httpClient->patch(guildEndpoint(guildId, "/emojis/" + QString::number(emojiId)), body, actionHandler("Renaming an emoji", std::move(callback)));
}

void Client::deleteGuildEmoji(Snowflake guildId, Snowflake emojiId, ActionCallback callback)
{
    httpClient->delete_(guildEndpoint(guildId, "/emojis/" + QString::number(emojiId)), actionHandler("Deleting an emoji", std::move(callback)));
}

void Client::fetchGuildStickers(Snowflake guildId, ResultCallback<QList<Sticker>> callback)
{
    httpClient->get(guildEndpoint(guildId, "/stickers"), {}, valueHandler<QList<Sticker>>("Fetching stickers", std::move(callback), parseArray<Sticker>));
}

void Client::createGuildSticker(Snowflake guildId, const QString &name, const QString &tags, const QString &description,
                                const FileUpload &file, const QByteArray &originalMd5, ResultCallback<Sticker> callback)
{
    FileUpload part = file;
    part.fieldName = "file";

    RequestOptions options;
    options.originalMd5 = originalMd5;
    httpClient->postForm(guildEndpoint(guildId, "/stickers"),
                         { { "name", name }, { "tags", tags }, { "description", description } }, { part }, options,
                         valueHandler<Sticker>("Uploading a sticker", std::move(callback),
                                               [](const QJsonDocument &doc) { return Sticker::fromJson(doc.object()); }));
}

void Client::modifyGuildSticker(Snowflake guildId, Snowflake stickerId, const QString &name, const QString &tags,
                                const QString &description, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("name", name);
    body.insert("tags", tags);
    body.insert("description", description);
    httpClient->patch(guildEndpoint(guildId, "/stickers/" + QString::number(stickerId)), body, actionHandler("Editing a sticker", std::move(callback)));
}

void Client::deleteGuildSticker(Snowflake guildId, Snowflake stickerId, ActionCallback callback)
{
    httpClient->delete_(guildEndpoint(guildId, "/stickers/" + QString::number(stickerId)), actionHandler("Deleting a sticker", std::move(callback)));
}

void Client::fetchAuditLog(Snowflake guildId, const AuditLogQuery &auditQuery, ResultCallback<AuditLog> callback)
{
    QUrlQuery query;
    query.addQueryItem("limit", QString::number(AuditLogQuery::PageSize));
    if (auditQuery.before.isValid())
        query.addQueryItem("before", QString::number(auditQuery.before));
    if (auditQuery.userId.isValid())
        query.addQueryItem("user_id", QString::number(auditQuery.userId));
    if (auditQuery.actionType)
        query.addQueryItem("action_type", QString::number(static_cast<int>(*auditQuery.actionType)));
    httpClient->get(guildEndpoint(guildId, "/audit-logs"), query,
                    valueHandler<AuditLog>("Fetching the audit log", std::move(callback),
                                           [](const QJsonDocument &doc) { return AuditLog::fromJson(doc.object()); }));
}

void Client::setState(Core::ConnectionState state)
{
    if (this->state != state) {
        this->state = state;
        emit stateChanged(state);
    }
}

} // namespace Discord
} // namespace Acheron