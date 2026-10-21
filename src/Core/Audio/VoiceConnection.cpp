#include "VoiceConnection.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Core {
namespace Audio {

using Discord::Voice::VoiceClient;

namespace {

constexpr int STOP_TIMEOUT_MS = 5000;

} // namespace

VoiceConnection::VoiceConnection(const Discord::Voice::ConnectionParams &params, QObject *parent)
    : QObject(parent), params(params), voiceClient(new VoiceClient(params)), connectionThread(new QThread(this))
{
    connectionThread->setObjectName("VoiceThread");

    relay(&VoiceClient::stateChanged, &VoiceConnection::stateChanged);
    relay(&VoiceClient::connected, &VoiceConnection::connected);
    relay(&VoiceClient::disconnected, &VoiceConnection::disconnected);
    relay(&VoiceClient::clientReachedServer, &VoiceConnection::clientReachedServer);
    relay(&VoiceClient::clientDisconnected, &VoiceConnection::clientDisconnected);
    relay(&VoiceClient::speakingReceived, &VoiceConnection::speakingReceived);
    relay(&VoiceClient::privacyCodeChanged, &VoiceConnection::privacyCodeChanged);
    relay(&VoiceClient::remoteVideoChanged, &VoiceConnection::remoteVideoChanged);
    relay(&VoiceClient::remoteVideoLayersChanged, &VoiceConnection::remoteVideoLayersChanged);
    relay(&VoiceClient::videoStatsUpdated, &VoiceConnection::videoStatsUpdated);
    relay(&VoiceClient::videoSendCodecChanged, &VoiceConnection::videoSendCodecChanged);
    relay(&VoiceClient::videoSendLayersKnown, &VoiceConnection::videoSendLayersKnown);
    relay(&VoiceClient::videoKeyframeRequested, &VoiceConnection::videoKeyframeRequested);
    relay(&VoiceClient::videoReceiversChanged, &VoiceConnection::videoReceiversChanged);
    relay(&VoiceClient::videoWantsReceived, &VoiceConnection::videoWantsReceived);
    relay(&VoiceClient::videoSendStatsUpdated, &VoiceConnection::videoSendStatsUpdated);
}

VoiceConnection::~VoiceConnection()
{
    stop();
    delete voiceClient;
}

template <typename Signal, typename Relay>
void VoiceConnection::relay(Signal signal, Relay relayed)
{
    connect(voiceClient, signal, this, [this, relayed](auto... arguments) {
        if (!stopped)
            (this->*relayed)(arguments...);
    });
}

void VoiceConnection::adopt(QObject *object)
{
    adopted.append(object);
}

bool VoiceConnection::start()
{
    if (!params.proxy.canRelayUdp()) {
        qCWarning(LogVoice) << "Refusing RTC connection: an HTTP proxy cannot relay its UDP";
        return false;
    }

    voiceClient->moveToThread(connectionThread);
    for (QObject *object : adopted)
        object->moveToThread(connectionThread);

    connectionThread->start();
    QMetaObject::invokeMethod(voiceClient, &VoiceClient::start);
    return true;
}

void VoiceConnection::stop(const std::function<void()> &beforeStop)
{
    stopped = true;
    if (!connectionThread->isRunning())
        return;

    QThread *home = QObject::thread();
    QMetaObject::invokeMethod(
            voiceClient,
            [this, &beforeStop, home]() {
                if (beforeStop)
                    beforeStop();
                voiceClient->stop();
                voiceClient->moveToThread(home);
                for (QObject *object : adopted)
                    object->moveToThread(home);
            },
            Qt::BlockingQueuedConnection);

    connectionThread->quit();
    if (!connectionThread->wait(STOP_TIMEOUT_MS)) {
        qCWarning(LogVoice) << "Voice thread did not stop in time, terminating";
        connectionThread->terminate();
        connectionThread->wait();
    }
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
