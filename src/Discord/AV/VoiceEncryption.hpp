#pragma once

#include <QByteArray>

#include <cstdint>

#include "VoiceEnums.hpp"

namespace Acheron {
namespace Discord {
namespace AV {

class VoiceEncryption
{
public:
    VoiceEncryption(EncryptionMode mode, const QByteArray &secretKey);

    /// Encrypts an audio payload for an outbound RTP packet.
    /// @param rtpHeader  The serialized RTP header bytes (used as AAD for AEAD modes).
    /// @param audioPayload  The raw audio data (e.g. 3-byte Opus silence).
    /// @return The encrypted section to append after the RTP header:
    ///         [ciphertext + auth tag] + [4-byte supplemental nonce (big-endian)].
    ///         Empty on failure.
    QByteArray encrypt(const QByteArray &rtpHeader, const QByteArray &audioPayload);

    /// Decrypts the encrypted section of an inbound RTP packet.
    /// @param rtpHeader  The serialized RTP header bytes (used as AAD for AEAD modes).
    /// @param encryptedSection  Everything after the RTP header in the received packet.
    /// @return The decrypted audio payload, or empty on failure.
    QByteArray decrypt(const QByteArray &rtpHeader, const QByteArray &encryptedSection);

    /// Initialize libsodium. Must be called before any encrypt/decrypt operations.
    /// Safe to call multiple times.
    static bool initialize();

    /// Check whether the given encryption mode is supported on this hardware.
    /// Must call initialize() first.
    static bool isModeAvailable(EncryptionMode mode);

private:
    QByteArray encryptAes256Gcm(const QByteArray &rtpHeader, const QByteArray &payload);
    QByteArray encryptXChacha20(const QByteArray &rtpHeader, const QByteArray &payload);
    QByteArray decryptAes256Gcm(const QByteArray &rtpHeader, const QByteArray &encrypted);
    QByteArray decryptXChacha20(const QByteArray &rtpHeader, const QByteArray &encrypted);

    EncryptionMode m_mode;
    QByteArray m_key;
    uint32_t m_nonce = 0;
};

} // namespace AV
} // namespace Discord
} // namespace Acheron
