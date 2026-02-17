#include "VoiceManager.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Core {
namespace AV {

VoiceManager::VoiceManager(Snowflake accountId, QObject *parent)
    : QObject(parent), accountId(accountId)
{
}

VoiceManager::~VoiceManager()
{
    if (voiceClient) {
        voiceClient->stop();
        delete voiceClient;
        voiceClient = nullptr;
    }
}

void VoiceManager::handleVoiceStateUpdate(const Discord::VoiceState &state)
{
    if (state.userId.get() != accountId)
        return;

    if (state.channelId.isNull() || !state.channelId.get().isValid()) {
        qCInfo(LogVoice) << "Voice state: disconnected from channel";
        voiceSessionId.clear();
        channelId = Snowflake::Invalid;
        guildId = Snowflake::Invalid;
        pending = {};

        if (voiceClient) {
            disconnect();
        }
        return;
    }

    voiceSessionId = state.sessionId.get();
    channelId = state.channelId.get();
    guildId = state.guildId.hasValue() ? state.guildId.get() : Snowflake::Invalid;
    selfMute = state.selfMute.get();
    selfDeaf = state.selfDeaf.get();

    qCInfo(LogVoice) << "Voice state: session =" << voiceSessionId
                     << "channel =" << channelId << "guild =" << guildId;

    pending.hasStateUpdate = true;

    if (pending.hasServerUpdate && pending.guildId == guildId) {
        connectToVoiceServer(pending.endpoint, pending.token);
        pending = {};
    }
}

void VoiceManager::handleVoiceServerUpdate(const Discord::VoiceServerUpdate &event)
{
    Snowflake eventGuildId = event.guildId.get();

    // "A null endpoint means that the voice server allocated has gone away and is trying to be reallocated.
    // You should attempt to disconnect from the currently connected voice server,
    // and not attempt to reconnect until a new voice server is allocated."
    if (event.endpoint.isNull() || event.endpoint.get().isEmpty()) {
        qCInfo(LogVoice) << "Voice server update with null endpoint, waiting for new one";
        return;
    }

    QString eventEndpoint = event.endpoint.get();
    QString eventToken = event.token.get();

    qCInfo(LogVoice) << "Voice server update: guild =" << eventGuildId
                     << "endpoint =" << eventEndpoint;

    if (voiceClient && guildId == eventGuildId) {
        qCInfo(LogVoice) << "Server-commanded voice reconnection";

        voiceClient->stop();
        delete voiceClient;
        voiceClient = nullptr;

        connectToVoiceServer(eventEndpoint, eventToken);
        return;
    }

    pending.token = eventToken;
    pending.endpoint = eventEndpoint;
    pending.guildId = eventGuildId;
    pending.hasServerUpdate = true;

    if (pending.hasStateUpdate && !voiceSessionId.isEmpty()) {
        connectToVoiceServer(eventEndpoint, eventToken);
        pending = {};
    }
}

void VoiceManager::disconnect()
{
    if (voiceClient) {
        voiceClient->stop();
        delete voiceClient;
        voiceClient = nullptr;
    }
    pending = {};
    emit voiceDisconnected();
    emit voiceStateChanged();
}

bool VoiceManager::isConnected() const
{
    return voiceClient &&
           voiceClient->state() == Discord::AV::VoiceClient::State::Connected;
}

void VoiceManager::connectToVoiceServer(const QString &endpoint, const QString &token)
{
    if (voiceSessionId.isEmpty()) {
        qCWarning(LogVoice) << "Cannot connect to voice: no session ID";
        return;
    }

    if (voiceClient) {
        voiceClient->stop();
        delete voiceClient;
        voiceClient = nullptr;
    }

    qCInfo(LogVoice) << "Creating VoiceClient for endpoint" << endpoint
                     << "guild" << guildId << "channel" << channelId;

    voiceClient = new Discord::AV::VoiceClient(endpoint, token, guildId, channelId, accountId, voiceSessionId, this);

    connect(voiceClient, &Discord::AV::VoiceClient::connected, this, &VoiceManager::onVoiceClientConnected);
    // defer with QueuedConnection because VoiceClient is deleted in this handler
    connect(voiceClient, &Discord::AV::VoiceClient::disconnected, this, &VoiceManager::onVoiceClientDisconnected, Qt::QueuedConnection);
    connect(voiceClient, &Discord::AV::VoiceClient::stateChanged, this, &VoiceManager::onVoiceClientStateChanged);

    voiceClient->start();
}

void VoiceManager::onVoiceClientConnected()
{
    qCInfo(LogVoice) << "Voice connection established for channel" << channelId;
    emit voiceConnected();
    emit voiceStateChanged();
}

void VoiceManager::onVoiceClientDisconnected()
{
    qCInfo(LogVoice) << "Voice client disconnected";

    if (voiceClient) {
        delete voiceClient;
        voiceClient = nullptr;
    }

    emit voiceDisconnected();
    emit voiceStateChanged();
}

void VoiceManager::onVoiceClientStateChanged(Discord::AV::VoiceClient::State state)
{
    qCDebug(LogVoice) << "VoiceManager: client state ->" << static_cast<int>(state);
    emit voiceStateChanged();
}

} // namespace AV
} // namespace Core
} // namespace Acheron
