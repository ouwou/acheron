#include "DaveSession.hpp"

#include <algorithm>
#include <utility>

#include "VoiceEnums.hpp"
#include "Core/Logging.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

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
    for (const auto &[ssrc, codec] : localVideoCodecs)
        daveEncryptor->AssignSsrcToCodec(ssrc, codec);

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

bool bringsNewKeys(const discord::dave::RosterMap &changes)
{
    return std::any_of(changes.begin(), changes.end(), [](const auto &member) { return !member.second.empty(); });
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
        // as the official client
        pendingTransitionBringsKeys = pendingTransitionBringsKeys || bringsNewKeys(*roster);
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
        pendingTransitionBringsKeys = true;
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
    if (std::exchange(pendingTransitionBringsKeys, false))
        emit newKeysNeedKeyframe();

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

size_t DaveSession::maxCiphertextSize(discord::dave::MediaType type, size_t frameSize)
{
    return daveEncryptor->GetMaxCiphertextByteSize(type, frameSize);
}

std::optional<size_t> DaveSession::encrypt(discord::dave::MediaType type, quint32 ssrc, std::span<const uint8_t> frame, std::span<uint8_t> out)
{
    size_t written = 0;
    const auto result = daveEncryptor->Encrypt(type, ssrc, discord::dave::ArrayView<const uint8_t>(frame.data(), frame.size()), discord::dave::ArrayView<uint8_t>(out.data(), out.size()), &written);
    if (result == discord::dave::IEncryptor::Success)
        return written;
    qCWarning(LogDave) << "DAVE encrypt failed: result =" << static_cast<int>(result) << "frameSize =" << frame.size();
    return std::nullopt;
}

size_t DaveSession::maxPlaintextSize(discord::dave::MediaType type, quint32 ssrc, size_t frameSize)
{
    return decryptorFor(ssrc)->GetMaxPlaintextByteSize(type, frameSize);
}

std::optional<size_t> DaveSession::decrypt(discord::dave::MediaType type, quint32 ssrc, std::span<const uint8_t> frame, std::span<uint8_t> out)
{
    size_t written = 0;
    const auto result = decryptorFor(ssrc)->Decrypt(type, discord::dave::ArrayView<const uint8_t>(frame.data(), frame.size()), discord::dave::ArrayView<uint8_t>(out.data(), out.size()), &written);
    if (result == discord::dave::IDecryptor::Success)
        return written;

    static constexpr const char *RESULT_NAMES[] = { "Success", "DecryptionFailure", "MissingKeyRatchet", "InvalidNonce", "MissingCryptor" };
    const auto index = static_cast<size_t>(result);
    const bool hasMagicMarker = frame.size() >= 2 && frame[frame.size() - 2] == 0xFA && frame.back() == 0xFA;
    qCDebug(LogDave) << "DAVE decrypt failed: SSRC =" << ssrc << "result =" << (index < std::size(RESULT_NAMES) ? RESULT_NAMES[index] : "Unknown") << "frameSize =" << frame.size() << "hasMagicMarker =" << hasMagicMarker;
    return std::nullopt;
}

discord::dave::IDecryptor *DaveSession::decryptorFor(quint32 ssrc)
{
    auto it = daveDecryptors.find(ssrc);
    if (it != daveDecryptors.end())
        return it->second.get();

    const auto user = ssrcMap.constFind(ssrc);
    const uint64_t uid = user == ssrcMap.constEnd() ? 0 : user.value();

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

void DaveSession::setLocalVideoCodec(quint32 ssrc, discord::dave::Codec codec)
{
    localVideoCodecs[ssrc] = codec;
    if (daveEncryptor)
        daveEncryptor->AssignSsrcToCodec(ssrc, codec);
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
        if (it.value() == uid)
            daveDecryptors.erase(it.key());
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

} // namespace Voice
} // namespace Discord
} // namespace Acheron
