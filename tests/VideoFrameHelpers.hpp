#pragma once

#include "Discord/Voice/VideoDepacketizer.hpp"

#include <bytes/bytes.h>
#include <dave/dave_interfaces.h>

#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <vector>

namespace VideoFrameHelpers {

inline std::vector<uint8_t> filler(size_t size, uint32_t seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> byte(1, 255);
    std::vector<uint8_t> bytes(size);
    for (uint8_t &value : bytes)
        value = static_cast<uint8_t>(byte(generator));
    return bytes;
}

inline bool reassemble(Acheron::Discord::Voice::VideoCodec codec, const std::vector<std::vector<uint8_t>> &payloads, std::vector<uint8_t> &frame)
{
    std::vector<std::span<const uint8_t>> spans;
    for (const std::vector<uint8_t> &payload : payloads)
        spans.emplace_back(payload);
    return Acheron::Discord::Voice::assembleFrame(codec, spans, frame);
}

class FixedKeyRatchet : public discord::dave::IKeyRatchet
{
public:
    discord::dave::EncryptionKey GetKey(discord::dave::KeyGeneration generation) noexcept override
    {
        std::vector<uint8_t> key(16, 0x5A);
        key[0] = static_cast<uint8_t>(generation);
        return discord::dave::EncryptionKey(std::move(key));
    }
    void DeleteKey(discord::dave::KeyGeneration) noexcept override {}
};

inline std::vector<uint8_t> encryptFrame(discord::dave::Codec codec, uint32_t ssrc, const std::vector<uint8_t> &frame)
{
    auto encryptor = discord::dave::CreateEncryptor();
    encryptor->SetKeyRatchet(std::make_unique<FixedKeyRatchet>());
    encryptor->AssignSsrcToCodec(ssrc, codec);

    std::vector<uint8_t> encrypted(encryptor->GetMaxCiphertextByteSize(discord::dave::MediaType::Video, frame.size()));
    size_t written = 0;
    const auto result = encryptor->Encrypt(discord::dave::MediaType::Video, ssrc, discord::dave::ArrayView<const uint8_t>(frame.data(), frame.size()), discord::dave::ArrayView<uint8_t>(encrypted.data(), encrypted.size()), &written);
    if (result != discord::dave::IEncryptor::Success)
        return {};
    encrypted.resize(written);
    return encrypted;
}

inline bool decryptFrame(const std::vector<uint8_t> &encrypted, std::vector<uint8_t> &frame)
{
    auto decryptor = discord::dave::CreateDecryptor();
    decryptor->TransitionToKeyRatchet(std::make_unique<FixedKeyRatchet>());

    frame.resize(decryptor->GetMaxPlaintextByteSize(discord::dave::MediaType::Video, encrypted.size()));
    size_t written = 0;
    const auto result = decryptor->Decrypt(discord::dave::MediaType::Video, discord::dave::ArrayView<const uint8_t>(encrypted.data(), encrypted.size()), discord::dave::ArrayView<uint8_t>(frame.data(), frame.size()), &written);
    frame.resize(written);
    return result == discord::dave::IDecryptor::Success;
}

} // namespace VideoFrameHelpers
