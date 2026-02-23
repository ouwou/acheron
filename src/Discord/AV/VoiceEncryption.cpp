#include "VoiceEncryption.hpp"

#include "Core/Logging.hpp"

#include <sodium.h>
#include <QtEndian>

#include <cstring>

namespace Acheron {
namespace Discord {
namespace AV {

static constexpr int SUPPLEMENTAL_NONCE_SIZE = 4;

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

QByteArray VoiceEncryption::encrypt(const QByteArray &rtpHeader, const QByteArray &audioPayload)
{
    switch (m_mode) {
    case EncryptionMode::AEAD_AES256_GCM_RTPSIZE:
        return encryptAes256Gcm(rtpHeader, audioPayload);
    case EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE:
        return encryptXChacha20(rtpHeader, audioPayload);
    default:
        qCWarning(LogVoice) << "Encrypt called with unknown encryption mode";
        return {};
    }
}

QByteArray VoiceEncryption::encryptAes256Gcm(const QByteArray &rtpHeader, const QByteArray &payload)
{
    // Nonce: 4-byte BE counter padded to 12 bytes
    uint8_t nonce[crypto_aead_aes256gcm_NPUBBYTES] = {};
    uint32_t nonceBE = qToBigEndian(m_nonce);
    std::memcpy(nonce, &nonceBE, SUPPLEMENTAL_NONCE_SIZE);

    // Ciphertext = payload + 16-byte auth tag
    QByteArray ciphertext(payload.size() + crypto_aead_aes256gcm_ABYTES, '\0');
    unsigned long long cipherLen = 0;

    int ret = crypto_aead_aes256gcm_encrypt(
            reinterpret_cast<unsigned char *>(ciphertext.data()), &cipherLen,
            reinterpret_cast<const unsigned char *>(payload.constData()), payload.size(),
            reinterpret_cast<const unsigned char *>(rtpHeader.constData()), rtpHeader.size(),
            nullptr, nonce,
            reinterpret_cast<const unsigned char *>(m_key.constData()));

    if (ret != 0) {
        qCWarning(LogVoice) << "AES-256-GCM encrypt failed";
        return {};
    }

    ciphertext.resize(static_cast<int>(cipherLen));
    ciphertext.append(reinterpret_cast<const char *>(&nonceBE), SUPPLEMENTAL_NONCE_SIZE);

    m_nonce++;
    return ciphertext;
}

QByteArray VoiceEncryption::encryptXChacha20(const QByteArray &rtpHeader, const QByteArray &payload)
{
    // Nonce: 4-byte BE counter padded to 24 bytes
    uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {};
    uint32_t nonceBE = qToBigEndian(m_nonce);
    std::memcpy(nonce, &nonceBE, SUPPLEMENTAL_NONCE_SIZE);

    QByteArray ciphertext(payload.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long cipherLen = 0;

    int ret = crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char *>(ciphertext.data()), &cipherLen,
            reinterpret_cast<const unsigned char *>(payload.constData()), payload.size(),
            reinterpret_cast<const unsigned char *>(rtpHeader.constData()), rtpHeader.size(),
            nullptr, nonce,
            reinterpret_cast<const unsigned char *>(m_key.constData()));

    if (ret != 0) {
        qCWarning(LogVoice) << "XChaCha20-Poly1305 encrypt failed";
        return {};
    }

    ciphertext.resize(static_cast<int>(cipherLen));
    ciphertext.append(reinterpret_cast<const char *>(&nonceBE), SUPPLEMENTAL_NONCE_SIZE);

    m_nonce++;
    return ciphertext;
}

QByteArray VoiceEncryption::decrypt(const QByteArray &rtpHeader, const QByteArray &encryptedSection)
{
    switch (m_mode) {
    case EncryptionMode::AEAD_AES256_GCM_RTPSIZE:
        return decryptAes256Gcm(rtpHeader, encryptedSection);
    case EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE:
        return decryptXChacha20(rtpHeader, encryptedSection);
    default:
        return {};
    }
}

QByteArray VoiceEncryption::decryptAes256Gcm(const QByteArray &rtpHeader, const QByteArray &encrypted)
{
    int minSize = static_cast<int>(crypto_aead_aes256gcm_ABYTES) + SUPPLEMENTAL_NONCE_SIZE;
    if (encrypted.size() < minSize) {
        qCDebug(LogVoice) << "AES-GCM decrypt: too short, need" << minSize << "got" << encrypted.size();
        return {};
    }

    // Extract 4-byte supplemental nonce from end of packet, place at start of 12-byte nonce
    uint8_t nonce[crypto_aead_aes256gcm_NPUBBYTES] = {};
    std::memcpy(nonce, encrypted.constData() + encrypted.size() - SUPPLEMENTAL_NONCE_SIZE,
                SUPPLEMENTAL_NONCE_SIZE);

    int cipherLen = encrypted.size() - SUPPLEMENTAL_NONCE_SIZE;
    QByteArray plaintext(cipherLen - static_cast<int>(crypto_aead_aes256gcm_ABYTES), '\0');
    unsigned long long plainLen = 0;

    int ret = crypto_aead_aes256gcm_decrypt(
            reinterpret_cast<unsigned char *>(plaintext.data()), &plainLen,
            nullptr,
            reinterpret_cast<const unsigned char *>(encrypted.constData()), cipherLen,
            reinterpret_cast<const unsigned char *>(rtpHeader.constData()), rtpHeader.size(),
            nonce,
            reinterpret_cast<const unsigned char *>(m_key.constData()));

    if (ret != 0) {
        qCDebug(LogVoice) << "AES-GCM decrypt failed: cipherLen =" << cipherLen
                          << "aadLen =" << rtpHeader.size()
                          << "nonce =" << QByteArray(reinterpret_cast<const char *>(nonce), crypto_aead_aes256gcm_NPUBBYTES).toHex(' ');
        return {};
    }

    plaintext.resize(static_cast<int>(plainLen));
    return plaintext;
}

QByteArray VoiceEncryption::decryptXChacha20(const QByteArray &rtpHeader, const QByteArray &encrypted)
{
    int minSize = static_cast<int>(crypto_aead_xchacha20poly1305_ietf_ABYTES) + SUPPLEMENTAL_NONCE_SIZE;
    if (encrypted.size() < minSize) {
        qCDebug(LogVoice) << "XChaCha20 decrypt: too short, need" << minSize << "got" << encrypted.size();
        return {};
    }

    // Extract 4-byte supplemental nonce from end of packet, place at start of 24-byte nonce
    uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {};
    std::memcpy(nonce, encrypted.constData() + encrypted.size() - SUPPLEMENTAL_NONCE_SIZE,
                SUPPLEMENTAL_NONCE_SIZE);

    int cipherLen = encrypted.size() - SUPPLEMENTAL_NONCE_SIZE;
    QByteArray plaintext(cipherLen - static_cast<int>(crypto_aead_xchacha20poly1305_ietf_ABYTES), '\0');
    unsigned long long plainLen = 0;

    int ret = crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(plaintext.data()), &plainLen,
            nullptr,
            reinterpret_cast<const unsigned char *>(encrypted.constData()), cipherLen,
            reinterpret_cast<const unsigned char *>(rtpHeader.constData()), rtpHeader.size(),
            nonce,
            reinterpret_cast<const unsigned char *>(m_key.constData()));

    if (ret != 0) {
        qCDebug(LogVoice) << "XChaCha20 decrypt failed: cipherLen =" << cipherLen
                          << "aadLen =" << rtpHeader.size()
                          << "nonce =" << QByteArray(reinterpret_cast<const char *>(nonce), crypto_aead_xchacha20poly1305_ietf_NPUBBYTES).toHex(' ');
        return {};
    }

    plaintext.resize(static_cast<int>(plainLen));
    return plaintext;
}

} // namespace AV
} // namespace Discord
} // namespace Acheron
