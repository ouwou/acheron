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

// RTP and RTCP share the socket; Discord tells them apart by the second byte, as rtcp-mux does.
[[nodiscard]] PacketKind classifyPacket(std::span<const uint8_t> packet);

// The rtpsize ciphers leave an RTCP packet's common header and sender SSRC readable.
static constexpr int RTCP_CLEAR_SIZE = 8;

struct ParsedRtp
{
    RtpHeader header;
    // What the rtpsize ciphers leave readable: fixed header, CSRCs and the extension preamble.
    int clearSize = 0;
    // The extension elements, which are encrypted and sit at the start of the payload.
    int extensionSize = 0;
};

[[nodiscard]] std::optional<ParsedRtp> parseRtp(std::span<const uint8_t> packet);

// How many bytes at the end of a payload are padding, for a packet whose header says it has
// some: the last byte holds the count, itself included (RFC 3550, 5.1). -1 when the count
// cannot be right.
[[nodiscard]] int rtpPaddingSize(std::span<const uint8_t> payload);

// Writes the fixed header, followed by the one-byte-form extension preamble when the header has
// extensions. Returns the bytes written, or -1 when `out` is too small or the header has CSRCs.
int writeRtpHeader(const RtpHeader &header, std::span<uint8_t> out, int extensionWords = 0);

struct RtpExtension
{
    uint8_t id = 0;
    std::span<const uint8_t> value;
};

// The header extensions every video packet carries, by the ids Discord gives them.
inline constexpr uint8_t TRANSMISSION_OFFSET_EXTENSION = 2;
inline constexpr uint8_t ABS_SEND_TIME_EXTENSION = 3;
inline constexpr uint8_t TRANSPORT_SEQUENCE_EXTENSION = 5;

// A retransmission's payload opens with the sequence number the packet first had (RFC 4588).
inline constexpr int RTX_HEADER_SIZE = 2;

// Looks one element up in a block of one-byte-form extension elements (RFC 8285).
[[nodiscard]] std::span<const uint8_t> findRtpExtension(std::span<const uint8_t> elements, uint8_t id);

// Writes elements in one-byte form, padded to a whole number of 32-bit words. Returns the bytes
// written, or -1 when an element cannot be expressed in that form or `out` is too small.
int writeRtpExtensions(std::span<const RtpExtension> elements, std::span<uint8_t> out);

} // namespace Voice
} // namespace Discord
} // namespace Acheron
