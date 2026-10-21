#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace Acheron {
namespace Core {
namespace Media {

// a read past the end fails, as does every read after it, so success is checked once at the end
class BitReader
{
public:
    explicit BitReader(std::span<const uint8_t> data) : data(data) {}

    [[nodiscard]] bool failed() const { return hasFailed; }
    [[nodiscard]] size_t position() const { return at; }
    void fail() { hasFailed = true; }

    void skip(size_t count)
    {
        if (hasFailed || data.size() * 8 - at < count)
            hasFailed = true;
        else
            at += count;
    }

    uint32_t bits(int count)
    {
        const size_t from = at;
        skip(static_cast<size_t>(count));
        uint32_t value = 0;
        for (size_t i = from; i < at; i++)
            value = (value << 1) | ((data[i / 8] >> (7 - i % 8)) & 1);
        return value;
    }

    bool flag() { return bits(1) != 0; }

    // Exp-Golomb, as H264 and H265 code them
    uint32_t ue()
    {
        int zeros = 0;
        while (!hasFailed && !flag()) {
            if (++zeros > MOST_LEADING_ZEROS)
                hasFailed = true;
        }
        return hasFailed ? 0 : (uint32_t(1) << zeros) - 1 + bits(zeros);
    }

    int32_t se()
    {
        const uint32_t code = ue();
        const int32_t magnitude = static_cast<int32_t>(code / 2 + code % 2);
        return code % 2 ? magnitude : -magnitude;
    }

private:
    static constexpr int MOST_LEADING_ZEROS = 31;

    std::span<const uint8_t> data;
    size_t at = 0;
    bool hasFailed = false;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
