#include "DaveSession.hpp"
#include "VoiceEnums.hpp"
#include "Core/Logging.hpp"

namespace Acheron {
namespace Discord {
namespace AV {

DaveSession::DaveSession(Core::Snowflake channelId, Core::Snowflake userId,
                         const QHash<quint32, uint64_t> &ssrcMap, QObject *parent)
    : QObject(parent),
      channelId(channelId),
      userId(userId),
      ssrcMap(ssrcMap)
{
    mlsSession = discord::dave::mls::CreateSession(
            nullptr,
            "",
            [](const std::string &reason, const std::string &detail) {
                qCWarning(LogDave) << "MLS failure:" << QString::fromStdString(reason)
                                   << QString::fromStdString(detail);
            });
}

DaveSession::~DaveSession() = default;

void DaveSession::init(uint16_t version)
{
    protocolVersion = version;
    pendingProtocolVersion = version;
    reinit();
}

void DaveSession::reinit()
{
    daveEnabled = false;
    daveDowngraded = false;

    connectedUserIds.insert(std::to_string(static_cast<uint64_t>(userId)));

    qCInfo(LogDave) << "Initializing DAVE session: channel =" << channelId
                    << "protocol version =" << protocolVersion
                    << "connectedUsers =" << connectedUserIds.size();

    mlsSession->Init(
            protocolVersion,
            static_cast<uint64_t>(channelId),
            std::to_string(static_cast<uint64_t>(userId)),
            transientKey);

    daveDecryptors.clear();
    pendingTransitionReady = false;

    daveEncryptor = discord::dave::CreateEncryptor();
    if (localSsrc != 0)
        daveEncryptor->AssignSsrcToCodec(localSsrc, discord::dave::Codec::Opus);

    auto keyPackage = mlsSession->GetMarshalledKeyPackage();
    if (!keyPackage.empty()) {
        QByteArray data(reinterpret_cast<const char *>(keyPackage.data()), keyPackage.size());
        qCInfo(LogDave) << "Sending MLS key package, size =" << data.size();
        emit sendKeyPackage(static_cast<int>(VoiceOpCode::DAVE_MLS_KEY_PACKAGE), data);
    }
}

void DaveSession::onExternalSenderPackage(const QByteArray &payload)
{
    qCInfo(LogDave) << "Received external sender package, size =" << payload.size();

    std::vector<uint8_t> data(payload.begin(), payload.end());
    mlsSession->SetExternalSender(data);
}

void DaveSession::onProposals(const QByteArray &payload)
{
    qCInfo(LogDave) << "Received proposals, size =" << payload.size()
                    << "connectedUsers =" << connectedUserIds.size();

    std::vector<uint8_t> data(payload.begin(), payload.end());
    auto response = mlsSession->ProcessProposals(std::move(data), connectedUserIds);

    if (response) {
        QByteArray commitWelcome(reinterpret_cast<const char *>(response->data()), response->size());
        qCInfo(LogDave) << "Sending commit+welcome, size =" << commitWelcome.size();
        emit sendCommitWelcome(
                static_cast<int>(VoiceOpCode::DAVE_MLS_COMMIT_WELCOME), commitWelcome);
    }
}

void DaveSession::onAnnounceCommitTransition(const QByteArray &payload)
{
    if (payload.size() < 2)
        return;

    const auto *raw = reinterpret_cast<const uint8_t *>(payload.constData());
    int transitionId = (raw[0] << 8) | raw[1];
    pendingTransitionId = transitionId;

    qCDebug(LogDave) << "Received announce commit transition: transitionId ="
                     << transitionId << "size =" << payload.size();

    std::vector<uint8_t> data(payload.begin() + 2, payload.end());
    auto result = mlsSession->ProcessCommit(std::move(data));

    if (auto *roster = std::get_if<discord::dave::RosterMap>(&result)) {
        qCInfo(LogDave) << "ProcessCommit succeeded, roster size =" << roster->size();
        pendingTransitionReady = true;
        emit sendReadyForTransition(transitionId);
        if (transitionId == 0)
            completeTransition();
    } else if (std::holds_alternative<discord::dave::failed_t>(result)) {
        qCWarning(LogDave) << "ProcessCommit failed (hard reject)";
        emit sendInvalidCommitWelcome(transitionId);
    } else {
        qCDebug(LogDave) << "ProcessCommit ignored (soft reject)";
    }
}

void DaveSession::onWelcome(const QByteArray &payload)
{
    if (payload.size() < 2)
        return;

    const auto *raw = reinterpret_cast<const uint8_t *>(payload.constData());
    int transitionId = (raw[0] << 8) | raw[1];
    pendingTransitionId = transitionId;

    qCInfo(LogDave) << "Received welcome: transitionId =" << transitionId
                    << "size =" << payload.size()
                    << "connectedUsers =" << connectedUserIds.size();

    std::vector<uint8_t> data(payload.begin() + 2, payload.end());
    auto roster = mlsSession->ProcessWelcome(std::move(data), connectedUserIds);

    if (roster) {
        qCInfo(LogDave) << "ProcessWelcome succeeded, roster size =" << roster->size();
        pendingTransitionReady = true;
        emit sendReadyForTransition(transitionId);
        if (transitionId == 0)
            completeTransition();
    } else {
        qCWarning(LogDave) << "ProcessWelcome failed";
        emit sendInvalidCommitWelcome(transitionId);
    }
}

void DaveSession::onPrepareTransition(int version, int transitionId)
{
    qCInfo(LogDave) << "Prepare transition: version =" << version
                    << "transitionId =" << transitionId;

    pendingTransitionId = transitionId;

    pendingProtocolVersion = static_cast<uint16_t>(version);

    emit sendReadyForTransition(transitionId);
}

void DaveSession::onExecuteTransition(int transitionId)
{
    qCInfo(LogDave) << "Execute transition: transitionId =" << transitionId;

    if (pendingProtocolVersion != protocolVersion) {
        protocolVersion = pendingProtocolVersion;
        if (protocolVersion == 0) {
            qCInfo(LogDave) << "DAVE downgrade to version 0, disabling";
            daveEnabled = false;
            daveDowngraded = true;
            emit daveStateChanged(false);
            return;
        }
    }

    if (!pendingTransitionReady) {
        qCWarning(LogDave) << "Execute transition" << transitionId
                           << "but no pending commit/welcome, reinitializing DAVE session";
        reinit();
        return;
    }

    completeTransition();
}

void DaveSession::completeTransition()
{
    if (!pendingTransitionReady) {
        qCWarning(LogDave) << "completeTransition but no pending commit/welcome!";
        return;
    }
    pendingTransitionReady = false;

    auto selfRatchet = mlsSession->GetKeyRatchet(std::to_string(userId));
    if (selfRatchet) {
        daveEncryptor->SetKeyRatchet(std::move(selfRatchet));
        qCInfo(LogDave) << "Refreshed encryptor key ratchet for new epoch";
    } else {
        qCWarning(LogDave) << "Could not get own key ratchet from MLS session";
    }

    for (auto &[ssrc, dec] : daveDecryptors) {
        auto it = ssrcMap.constFind(ssrc);
        if (it == ssrcMap.constEnd())
            continue;
        auto ratchet = mlsSession->GetKeyRatchet(std::to_string(it.value()));
        if (ratchet) {
            dec->TransitionToKeyRatchet(std::move(ratchet));
            qCDebug(LogDave) << "Refreshed decryptor key ratchet for SSRC =" << ssrc;
        }
    }

    if (!daveEnabled) {
        daveEnabled = true;
        daveDowngraded = false;
        qCInfo(LogDave) << "DAVE encryption enabled";
    }

    emit daveStateChanged(true);

    pendingTransitionId = -1;
}

void DaveSession::onPrepareEpoch(int version, int epoch)
{
    qCInfo(LogDave) << "Prepare epoch: version =" << version << "epoch =" << epoch;

    if (epoch == 1) {
        protocolVersion = static_cast<uint16_t>(version);
        reinit();
    }
}

discord::dave::IEncryptor *DaveSession::encryptor()
{
    return daveEncryptor.get();
}

discord::dave::IDecryptor *DaveSession::getOrCreateDecryptor(quint32 ssrc, uint64_t uid)
{
    auto it = daveDecryptors.find(ssrc);
    if (it != daveDecryptors.end())
        return it->second.get();

    auto decryptor = discord::dave::CreateDecryptor();

    bool hasRatchet = false;
    if (uid != 0) {
        auto keyRatchet = mlsSession->GetKeyRatchet(std::to_string(uid));
        if (keyRatchet) {
            decryptor->TransitionToKeyRatchet(std::move(keyRatchet));
            hasRatchet = true;
        }
    }

    auto *ptr = decryptor.get();
    daveDecryptors.emplace(ssrc, std::move(decryptor));

    qCDebug(LogDave) << "Created decryptor for SSRC =" << ssrc
                     << "hasRatchet =" << hasRatchet;
    return ptr;
}

void DaveSession::setLocalSsrc(quint32 ssrc)
{
    localSsrc = ssrc;
    if (daveEncryptor)
        daveEncryptor->AssignSsrcToCodec(ssrc, discord::dave::Codec::Opus);
}

void DaveSession::addConnectedUser(const std::string &id)
{
    connectedUserIds.insert(id);
}

void DaveSession::removeConnectedUser(const std::string &id)
{
    connectedUserIds.erase(id);

    uint64_t uid = std::stoull(id);
    for (auto it = ssrcMap.constBegin(); it != ssrcMap.constEnd(); ++it) {
        if (it.value() == uid) {
            daveDecryptors.erase(it.key());
            break;
        }
    }
}

void DaveSession::getPairwiseFingerprint(const std::string &userId, FingerprintCallback callback) const
{
    if (!mlsSession || !daveEnabled) {
        callback({});
        return;
    }

    mlsSession->GetPairwiseFingerprint(0, userId, std::move(callback));
}

std::vector<uint8_t> DaveSession::lastEpochAuthenticator() const
{
    if (!mlsSession || !daveEnabled)
        return {};
    return mlsSession->GetLastEpochAuthenticator();
}

void DaveSession::applyKeyRatchetForSsrc(quint32 ssrc, uint64_t uid)
{
    auto it = daveDecryptors.find(ssrc);
    if (it == daveDecryptors.end())
        return;

    auto keyRatchet = mlsSession->GetKeyRatchet(std::to_string(uid));
    if (keyRatchet) {
        it->second->TransitionToKeyRatchet(std::move(keyRatchet));
        qCInfo(LogDave) << "Applied key ratchet for SSRC =" << ssrc << "user =" << uid;
    }
}

} // namespace AV
} // namespace Discord
} // namespace Acheron
