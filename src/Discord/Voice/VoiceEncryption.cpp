#include "VoiceEncryption.hpp"

#include "PacketBuffer.hpp"

#include "Core/Logging.hpp"

#include <sodium.h>
#include <QtEndian>

#include <cstring>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

constexpr int SUPPLEMENTAL_NONCE_SIZE = 4;
constexpr int TAG_SIZE = VoiceEncryption::TRAILER_SIZE - SUPPLEMENTAL_NONCE_SIZE;
constexpr int LARGEST_NONCE_SIZE = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;

static_assert(crypto_aead_aes256gcm_ABYTES == TAG_SIZE);
static_assert(crypto_aead_xchacha20poly1305_ietf_ABYTES == TAG_SIZE);
static_assert(crypto_aead_aes256gcm_NPUBBYTES <= LARGEST_NONCE_SIZE);

} // namespace

VoiceEncryption::VoiceEncryption(EncryptionMode mode, const QByteArray &secretKey)
    : m_mode(mode), m_key(secretKey)
{
}

bool VoiceEncryption::initialize()
{
    static bool success = []() {
        int result = sodium_init();
        if (result < 0) {
            qCCritical(LogVoice) << "Failed to initialize libsodium";
            return false;
        }
        return true;
    }();
    return success;
}

bool VoiceEncryption::isModeAvailable(EncryptionMode mode)
{
    switch (mode) {
    case EncryptionMode::AEAD_AES256_GCM_RTPSIZE:
        return crypto_aead_aes256gcm_is_available() != 0;
    case EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE:
        return true;
    default:
        return false;
    }
}

bool VoiceEncryption::seal(PacketBuffer &packet, int clearSize)
{
    if (clearSize < 0 || clearSize > packet.size() || packet.tailroom() < TRAILER_SIZE)
        return false;

    uint8_t nonce[LARGEST_NONCE_SIZE] = {};
    const uint32_t counter = qToBigEndian(m_nonce);
    std::memcpy(nonce, &counter, SUPPLEMENTAL_NONCE_SIZE);

    const auto *key = reinterpret_cast<const unsigned char *>(m_key.constData());
    uint8_t *body = packet.data() + clearSize;
    const int bodySize = packet.size() - clearSize;
    unsigned long long sealedSize = 0;

    int result = -1;
    switch (m_mode) {
    case EncryptionMode::AEAD_AES256_GCM_RTPSIZE:
        result = crypto_aead_aes256gcm_encrypt(body, &sealedSize, body, bodySize, packet.data(), clearSize, nullptr, nonce, key);
        break;
    case EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE:
        result = crypto_aead_xchacha20poly1305_ietf_encrypt(body, &sealedSize, body, bodySize, packet.data(), clearSize, nullptr, nonce, key);
        break;
    default:
        break;
    }
    if (result != 0)
        return false;

    packet.resize(clearSize + static_cast<int>(sealedSize));
    packet.append({ nonce, SUPPLEMENTAL_NONCE_SIZE });

    m_nonce++;
    return true;
}

bool VoiceEncryption::open(PacketBuffer &packet, int clearSize)
{
    if (clearSize < 0 || packet.size() < clearSize + TRAILER_SIZE)
        return false;

    uint8_t nonce[LARGEST_NONCE_SIZE] = {};
    std::memcpy(nonce, packet.data() + packet.size() - SUPPLEMENTAL_NONCE_SIZE, SUPPLEMENTAL_NONCE_SIZE);

    const auto *key = reinterpret_cast<const unsigned char *>(m_key.constData());
    uint8_t *body = packet.data() + clearSize;
    const int sealedSize = packet.size() - clearSize - SUPPLEMENTAL_NONCE_SIZE;
    unsigned long long plainSize = 0;

    int result = -1;
    switch (m_mode) {
    case EncryptionMode::AEAD_AES256_GCM_RTPSIZE:
        result = crypto_aead_aes256gcm_decrypt(body, &plainSize, nullptr, body, sealedSize, packet.data(), clearSize, nonce, key);
        break;
    case EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE:
        result = crypto_aead_xchacha20poly1305_ietf_decrypt(body, &plainSize, nullptr, body, sealedSize, packet.data(), clearSize, nonce, key);
        break;
    default:
        break;
    }
    if (result != 0)
        return false;

    packet.resize(clearSize + static_cast<int>(plainSize));
    return true;
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
