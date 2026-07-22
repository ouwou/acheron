#include "Client.hpp"

#include <QDebug>
#include <QJsonObject>

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

} // namespace

Client::Client(const QString &token, const QString &gatewayUrl, const QString &baseUrl,
               CaptchaResolver *captchaResolver, QObject *parent)
    : QObject(parent), token(token), baseUrl(baseUrl)
{
    identity.regenerateClientHeartbeatSessionId();

    gateway = new Gateway(token, gatewayUrl, identity, this);
    httpClient = new HttpClient(baseUrl, token, identity, captchaResolver, this);

    connect(gateway, &Gateway::connected, this, &Client::onConnected);
    connect(gateway, &Gateway::disconnected, this, &Client::onDisconnected);

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
    connect(gateway, &Gateway::gatewayGuildMembersChunk, this, &Client::guildMembersChunk);
    connect(gateway, &Gateway::gatewayGuildMemberUpdate, this, &Client::guildMemberUpdated);
    connect(gateway, &Gateway::gatewayGuildRoleCreate, this, &Client::onGatewayGuildRoleCreate);
    connect(gateway, &Gateway::gatewayGuildRoleUpdate, this, &Client::onGatewayGuildRoleUpdate);
    connect(gateway, &Gateway::gatewayGuildRoleDelete, this, &Client::onGatewayGuildRoleDelete);
    connect(gateway, &Gateway::gatewayMessageAck, this, &Client::messageAcked);
    connect(gateway, &Gateway::gatewayMessageReactionAdd, this, &Client::messageReactionAdd);
    connect(gateway, &Gateway::gatewayMessageReactionAddMany, this, &Client::messageReactionAddMany);
    connect(gateway, &Gateway::gatewayMessageReactionRemove, this, &Client::messageReactionRemove);
    connect(gateway, &Gateway::gatewayMessageReactionRemoveAll, this, &Client::messageReactionRemoveAll);
    connect(gateway, &Gateway::gatewayMessageReactionRemoveEmoji, this, &Client::messageReactionRemoveEmoji);
    connect(gateway, &Gateway::gatewayUserGuildSettingsUpdate, this,
            &Client::userGuildSettingsUpdated);
    connect(gateway, &Gateway::gatewayGuildMemberListUpdate, this, &Client::guildMemberListUpdate);
    connect(gateway, &Gateway::gatewayVoiceStateUpdate, this, &Client::voiceStateUpdated);
    connect(gateway, &Gateway::gatewayVoiceServerUpdate, this, &Client::voiceServerUpdated);
    connect(gateway, &Gateway::gatewayRelationshipAdd, this, &Client::relationshipAdded);
    connect(gateway, &Gateway::gatewayRelationshipUpdate, this, &Client::relationshipUpdated);
    connect(gateway, &Gateway::gatewayRelationshipRemove, this, &Client::relationshipRemoved);
    connect(gateway, &Gateway::gatewayUserNoteUpdate, this, &Client::userNoteUpdated);
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
    setState(Core::ConnectionState::Disconnecting);
    gateway->stop();
}

[[nodiscard]] Core::ConnectionState Client::getState() const
{
    return state;
}

void Client::fetchLatestMessages(Snowflake channelId, int limit, MessagesCallback callback)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/messages";
    QUrlQuery query;
    query.addQueryItem("limit", QString::number(limit));

    httpClient->get(endpoint, query, [this, channelId, callback](const HttpResponse &response) {
        if (!response.success) {
            qCWarning(LogDiscord) << "Failed to fetch latest messages: " << response.error;
            callback({ {}, "Failed to fetch latest messages: " + response.error });
            return;
        }

        QList<Message> results;
        QJsonArray arr = QJsonDocument::fromJson(response.body).array();
        for (const QJsonValue &val : arr)
            results.append(Message::fromJson(val.toObject()));

        callback({ results });
    });
}

void Client::fetchHistory(Snowflake channelId, Snowflake beforeId, int limit,
                          MessagesCallback callback)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/messages";
    QUrlQuery query;
    query.addQueryItem("before", QString::number(beforeId));
    query.addQueryItem("limit", QString::number(limit));

    httpClient->get(endpoint, query, [this, channelId, callback](const HttpResponse &response) {
        if (!response.success) {
            qCWarning(LogDiscord) << "Failed to fetch history: " << response.error;
            callback({ {}, "Failed to fetch history: " + response.error });
            return;
        }

        QList<Message> results;
        QJsonArray arr = QJsonDocument::fromJson(response.body).array();
        for (const QJsonValue &val : arr)
            results.append(Message::fromJson(val.toObject()));

        callback({ results });
    });
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

    const QByteArray binary = QByteArray::fromBase64(data.userSettingsProto->toUtf8());
    Proto::ProtoReader reader(binary);
    settings = Proto::PreloadedUserSettings::fromProto(reader);

    if ((!settings.guildFolders.has_value() || settings.guildFolders->folders.isEmpty()) && data.userSettings.hasValue() && !data.userSettings->guildFolders->isEmpty())
        settings.guildFolders = guildFoldersFromLegacy(data.userSettings->guildFolders.get());

    me = data.user;

    emit ready(data);
}

void Client::onGatewayReadySupplemental(const ReadySupplemental &data)
{
    emit readySupplemental(data);
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
            httpClient->post(endpoint, withFiles, [this, nonce](const HttpResponse &response) {
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
    httpClient->post(endpoint, payload, [this, channelId, nonce](const HttpResponse &response) {
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

void Client::addReaction(Snowflake channelId, Snowflake messageId, const QString &emoji,
                         bool isBurst)
{
    QString encoded = QUrl::toPercentEncoding(emoji, ":");
    int type = isBurst ? 1 : 0;
    QString endpoint = "/channels/" + QString::number(channelId) + "/messages/" +
                       QString::number(messageId) + "/reactions/" + encoded +
                       "/%40me?location=Message%20Inline%20Button&type=" + QString::number(type);

    httpClient->put(endpoint, QJsonObject{}, [this, channelId, messageId](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to add reaction on message" << messageId
                                  << "in channel" << channelId << ":" << response.error;
    });
}

void Client::removeReaction(Snowflake channelId, Snowflake messageId, const QString &emoji,
                            bool isBurst)
{
    QString encoded = QUrl::toPercentEncoding(emoji, ":");
    QString endpoint = "/channels/" + QString::number(channelId) + "/messages/" +
                       QString::number(messageId) + "/reactions/" + encoded + "/%40me";
    if (isBurst)
        endpoint += "?burst=true";
    else
        endpoint += "?type=0";

    httpClient->delete_(endpoint, [this, channelId, messageId](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to remove reaction on message" << messageId
                                  << "in channel" << channelId << ":" << response.error;
    });
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

void Client::ackBulk(const QList<AckEntry> &entries)
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
    httpClient->post("/read-states/ack-bulk", payload, [this](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Failed to bulk ack:" << response.error;
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

void Client::requestGuildMembers(Snowflake guildId, const QList<Snowflake> &userIds)
{
    gateway->requestGuildMembers(guildId, userIds);
}

[[nodiscard]] const Proto::PreloadedUserSettings &Client::getSettings() const
{
    return settings;
}

[[nodiscard]] const User &Client::getMe() const
{
    return me;
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