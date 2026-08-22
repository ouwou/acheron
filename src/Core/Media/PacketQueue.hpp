#pragma once

#ifdef ACHERON_HAVE_FFMPEG

#include <QtGlobal>

#include <cstddef>
#include <deque>

extern "C" {
#include <libavcodec/packet.h>
}

namespace Acheron {
namespace Core {
namespace Media {

// lock with Player's mutex
class PacketQueue
{
public:
    static constexpr size_t MaxBufferedBytes = 8 * 1024 * 1024;

    ~PacketQueue() { clearLocked(); }

    [[nodiscard]] bool emptyLocked() const { return entries.empty(); }
    [[nodiscard]] bool fullLocked() const { return bufferedBytes >= MaxBufferedBytes; }
    [[nodiscard]] bool frontIsFlushLocked() const { return !entries.empty() && !entries.front().packet; }

    void pushLocked(AVPacket *packet)
    {
        bufferedBytes += static_cast<size_t>(packet->size);
        entries.push_back({ packet, -1 });
    }

    void pushFlushLocked(qint64 targetMs) { entries.push_back({ nullptr, targetMs }); }

    [[nodiscard]] qint64 takeFlushLocked()
    {
        const qint64 target = entries.front().flushToMs;
        entries.pop_front();
        return target;
    }

    AVPacket *takePacketLocked()
    {
        AVPacket *packet = entries.front().packet;
        entries.pop_front();
        bufferedBytes -= static_cast<size_t>(packet->size);
        return packet;
    }

    void clearLocked()
    {
        for (Entry &entry : entries) {
            if (entry.packet)
                av_packet_free(&entry.packet);
        }
        entries.clear();
        bufferedBytes = 0;
    }

private:
    struct Entry
    {
        AVPacket *packet = nullptr;
        qint64 flushToMs = -1;
    };

    std::deque<Entry> entries;
    size_t bufferedBytes = 0;
};

} // namespace Media
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
