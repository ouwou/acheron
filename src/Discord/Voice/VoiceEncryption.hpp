#pragma once

#include <QByteArray>

#include <cstdint>

#include "VoiceEnums.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

class PacketBuffer;

// Discord's rtpsize transport cipher. RTP and RTCP share key and nonce counter and differ only in how
// many leading bytes stay readable
class VoiceEncryption
{
public:
    VoiceEncryption(EncryptionMode mode, const QByteArray &secretKey);

    static constexpr int TRAILER_SIZE = 16 + 4;

    bool seal(PacketBuffer &packet, int clearSize);

    bool open(PacketBuffer &packet, int clearSize);

    // safe to call repeatedly; must have succeeded before any other use
    static bool initialize();
    static bool isModeAvailable(EncryptionMode mode);

private:
    EncryptionMode m_mode;
    QByteArray m_key;
    uint32_t m_nonce = 0;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
