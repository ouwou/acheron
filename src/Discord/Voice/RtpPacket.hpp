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

// Discord tells RTP from RTCP by the second byte, as rtcp-mux does
[[nodiscard]] PacketKind classifyPacket(std::span<const uint8_t> packet);

// the rtpsize ciphers leave an RTCP packet's common header and sender SSRC readable
inline constexpr int RTCP_CLEAR_SIZE = 8;

struct ParsedRtp
{
    RtpHeader header;
    // what the rtpsize ciphers leave readable
    int clearSize = 0;
    int extensionSize = 0;
};

[[nodiscard]] std::optional<ParsedRtp> parseRtp(std::span<const uint8_t> packet);

// RFC 3550, 5.1
[[nodiscard]] std::optional<int> rtpPaddingSize(std::span<const uint8_t> payload);

int writeRtpHeader(const RtpHeader &header, std::span<uint8_t> out, int extensionWords = 0);

struct RtpExtension
{
    uint8_t id = 0;
    std::span<const uint8_t> value;
};

// Discord's ids
inline constexpr uint8_t TRANSMISSION_OFFSET_EXTENSION = 2;
inline constexpr uint8_t ABS_SEND_TIME_EXTENSION = 3;
inline constexpr uint8_t TRANSPORT_SEQUENCE_EXTENSION = 5;

inline constexpr int VIDEO_CLOCK_KHZ = 90;

// 16-bit sequence numbers made monotonic, starting high enough that numbers before the first stay positive
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

    static constexpr int64_t BASE = int64_t(1) << 20;

private:
    std::optional<int64_t> highest;
};

// RFC 4588
inline constexpr int RTX_HEADER_SIZE = 2;

// RFC 8285, one-byte form
[[nodiscard]] std::span<const uint8_t> findRtpExtension(std::span<const uint8_t> elements, uint8_t id);

int writeRtpExtensions(std::span<const RtpExtension> elements, std::span<uint8_t> out);

} // namespace Voice
} // namespace Discord
} // namespace Acheron
