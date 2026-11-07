#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace Acheron {
namespace Discord {
namespace Voice {

struct RtpHeader
{
    bool padding = false;
    bool extension = false;
    uint8_t csrcCount = 0;
    bool marker = false;
    uint8_t payloadType = 0;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;

    static constexpr int FIXED_SIZE = 12;
    static constexpr int EXTENSION_PREAMBLE_SIZE = 4;
};

enum class PacketKind {
    Rtp,
    Rtcp,
    Unknown,
};

[[nodiscard]] PacketKind classifyPacket(std::span<const uint8_t> packet);

// the rtpsize ciphers leave the RTCP common header and sender SSRC in the clear
inline constexpr int RTCP_CLEAR_SIZE = 8;

struct ParsedRtp
{
    RtpHeader header;
    int clearSize = 0;
    int extensionSize = 0;
};

[[nodiscard]] std::optional<ParsedRtp> parseRtp(std::span<const uint8_t> packet);

[[nodiscard]] std::optional<int> rtpPaddingSize(std::span<const uint8_t> payload);

int writeRtpHeader(const RtpHeader &header, std::span<uint8_t> out, int extensionWords = 0);

struct RtpExtension
{
    uint8_t id = 0;
    std::span<const uint8_t> value;
};

// fixed by discord_voice.node, never negotiated
inline constexpr uint8_t TRANSMISSION_OFFSET_EXTENSION = 2;
inline constexpr uint8_t ABS_SEND_TIME_EXTENSION = 3;
inline constexpr uint8_t VIDEO_ORIENTATION_EXTENSION = 4;
inline constexpr uint8_t TRANSPORT_SEQUENCE_EXTENSION = 5;

inline constexpr int VIDEO_CLOCK_KHZ = 90;

class SequenceUnwrapper
{
public:
    [[nodiscard]] int64_t unwrap(uint16_t sequence) const
    {
        if (!highest)
            return BASE + sequence;
        return *highest + static_cast<int16_t>(static_cast<uint16_t>(sequence - static_cast<uint16_t>(*highest)));
    }

    void advance(int64_t sequence)
    {
        if (!highest || sequence > *highest)
            highest = sequence;
    }

    void reset() { highest.reset(); }

    [[nodiscard]] std::optional<int64_t> highestSeen() const { return highest; }

    // far enough above zero that numbers older than the first stay positive
    static constexpr int64_t BASE = int64_t(1) << 20;

private:
    std::optional<int64_t> highest;
};

inline constexpr int RTX_HEADER_SIZE = 2;

// the one-byte form only, which is all discord sends
[[nodiscard]] std::span<const uint8_t> findRtpExtension(std::span<const uint8_t> elements, uint8_t id);

int writeRtpExtensions(std::span<const RtpExtension> elements, std::span<uint8_t> out);

} // namespace Voice
} // namespace Discord
} // namespace Acheron
