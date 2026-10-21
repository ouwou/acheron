#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace Acheron {
namespace Discord {
namespace Voice {

// HEADROOM spare bytes before the content let a SOCKS5 relay header be written in front, and the
// cipher's tag and nonce behind, without moving the payload
class PacketBuffer
{
public:
    static constexpr int CAPACITY = 2048;
    static constexpr int HEADROOM = 32;
    static constexpr int MAX_CONTENT = CAPACITY - HEADROOM;

    void clear()
    {
        start = HEADROOM;
        length = 0;
    }

    [[nodiscard]] uint8_t *data() { return storage + start; }
    [[nodiscard]] const uint8_t *data() const { return storage + start; }
    [[nodiscard]] int size() const { return length; }
    [[nodiscard]] int tailroom() const { return CAPACITY - start - length; }
    [[nodiscard]] int headroom() const { return start; }

    [[nodiscard]] std::span<uint8_t> bytes() { return { data(), static_cast<size_t>(length) }; }
    [[nodiscard]] std::span<const uint8_t> bytes() const { return { data(), static_cast<size_t>(length) }; }
    [[nodiscard]] std::span<uint8_t> writable() { return { data(), static_cast<size_t>(CAPACITY - start) }; }

    bool resize(int newLength)
    {
        if (newLength < 0 || newLength > CAPACITY - start)
            return false;
        length = newLength;
        return true;
    }

    bool append(std::span<const uint8_t> bytes)
    {
        if (static_cast<int>(bytes.size()) > tailroom())
            return false;
        std::memcpy(data() + length, bytes.data(), bytes.size());
        length += static_cast<int>(bytes.size());
        return true;
    }

    bool dropFront(int count)
    {
        if (count < 0 || count > length)
            return false;
        start += count;
        length -= count;
        return true;
    }

    bool growFront(int count)
    {
        if (count < 0 || count > start)
            return false;
        start -= count;
        length += count;
        return true;
    }

private:
    uint8_t storage[CAPACITY];
    int start = HEADROOM;
    int length = 0;
};

// not thread safe: a pool and its handles stay on one thread
class PacketPool
{
public:
    class Recycler
    {
    public:
        void operator()(PacketBuffer *buffer) const;

    private:
        friend class PacketPool;
        struct Idle;
        std::shared_ptr<Idle> idle;
    };
    using Handle = std::unique_ptr<PacketBuffer, Recycler>;

    PacketPool();

    [[nodiscard]] Handle acquire();
    [[nodiscard]] int idleCount() const;

    static constexpr int MAX_IDLE = 256;

private:
    std::shared_ptr<Recycler::Idle> idle;
};

struct PacketPool::Recycler::Idle
{
    std::vector<std::unique_ptr<PacketBuffer>> buffers;
};

inline PacketPool::PacketPool() : idle(std::make_shared<Recycler::Idle>()) { }

inline PacketPool::Handle PacketPool::acquire()
{
    Recycler recycler;
    recycler.idle = idle;

    PacketBuffer *buffer;
    if (idle->buffers.empty()) {
        buffer = new PacketBuffer;
    } else {
        buffer = idle->buffers.back().release();
        idle->buffers.pop_back();
        buffer->clear();
    }
    return Handle(buffer, std::move(recycler));
}

inline int PacketPool::idleCount() const
{
    return static_cast<int>(idle->buffers.size());
}

inline void PacketPool::Recycler::operator()(PacketBuffer *buffer) const
{
    if (idle && static_cast<int>(idle->buffers.size()) < MAX_IDLE)
        idle->buffers.emplace_back(buffer);
    else
        delete buffer;
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
