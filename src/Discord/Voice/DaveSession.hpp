#pragma once

#include <QObject>
#include <QHash>

#include <functional>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <dave/dave_interfaces.h>

#include "Core/Snowflake.hpp"

namespace mlspp {
struct SignaturePrivateKey;
}

namespace Acheron {
namespace Discord {
namespace Voice {

// Whether a commit's changes to the roster give anyone a key: someone added, or someone whose
// key changed. One who left is in it without a key.
[[nodiscard]] bool bringsNewKeys(const discord::dave::RosterMap &changes);

class DaveSession : public QObject
{
    Q_OBJECT
public:
    DaveSession(Core::Snowflake channelId, Core::Snowflake userId,
                const QHash<quint32, uint64_t> &ssrcMap, QObject *parent = nullptr);
    ~DaveSession() override;

    void init(uint16_t protocolVersion);

    // binary
    void onExternalSenderPackage(const QByteArray &payload);
    void onProposals(const QByteArray &payload);
    void onAnnounceCommitTransition(const QByteArray &payload);
    void onWelcome(const QByteArray &payload);

    // json
    void onPrepareTransition(int protocolVersion, int transitionId);
    void onExecuteTransition(int transitionId);
    void onPrepareEpoch(int protocolVersion, int epoch);

    discord::dave::IEncryptor *encryptor();
    discord::dave::IDecryptor *getOrCreateDecryptor(quint32 ssrc, uint64_t userId);

    bool isDaveEnabled() const { return daveEnabled; }

    bool isDowngraded() const { return daveDowngraded; }

    using FingerprintCallback = std::function<void(const std::vector<uint8_t> &)>;
    void getPairwiseFingerprint(const std::string &userId, FingerprintCallback callback) const;

    std::vector<uint8_t> lastEpochAuthenticator() const;

    void setLocalSsrc(quint32 ssrc);
    // An SSRC this client sends video on, one per layer. The encryptor goes by the codec to tell
    // which parts of a frame have to stay readable for it to be packetized. The SSRCs that carry
    // retransmissions are not named: they repeat packets of frames encrypted already.
    void setLocalVideoCodec(quint32 ssrc, discord::dave::Codec codec);
    void addConnectedUser(const std::string &userId);
    void removeConnectedUser(const std::string &userId);
    void applyKeyRatchetForSsrc(quint32 ssrc, uint64_t userId);

signals:
    void sendKeyPackage(int opcode, const QByteArray &data);
    void sendCommitWelcome(int opcode, const QByteArray &data);
    void sendReadyForTransition(int transitionId);
    void sendInvalidCommitWelcome(int transitionId);
    void daveStateChanged(bool enabled);
    // A transition was carried out that gave someone a key they did not have: a commit that
    // added or re-keyed a member, or a welcome. They can make nothing of what this client
    // sends until its next key frame, and the official client asks its encoder for one then.
    void newKeysNeedKeyframe();

private:
    void reinit();
    void completeTransition();

    std::unique_ptr<discord::dave::mls::ISession> mlsSession;
    std::unique_ptr<discord::dave::IEncryptor> daveEncryptor;
    std::unordered_map<quint32, std::unique_ptr<discord::dave::IDecryptor>> daveDecryptors;

    uint16_t protocolVersion = 0;
    uint16_t pendingProtocolVersion = 0;
    Core::Snowflake channelId;
    Core::Snowflake userId;
    quint32 localSsrc = 0;
    std::map<quint32, discord::dave::Codec> localVideoCodecs;

    std::set<std::string> connectedUserIds;
    int pendingTransitionId = -1;
    bool daveEnabled = false;
    bool daveDowngraded = false;
    bool pendingTransitionReady = false;
    bool pendingTransitionBringsKeys = false;

    const QHash<quint32, uint64_t> &ssrcMap;

    std::shared_ptr<::mlspp::SignaturePrivateKey> transientKey;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
