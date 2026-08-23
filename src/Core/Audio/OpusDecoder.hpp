#pragma once

#include <QByteArray>
#include <QVector>

#include <opus.h>

namespace Acheron {
namespace Core {
namespace Audio {

class OpusDecoder
{
public:
    OpusDecoder();
    ~OpusDecoder();

    OpusDecoder(const OpusDecoder &) = delete;
    OpusDecoder &operator=(const OpusDecoder &) = delete;

    bool init(int sampleRate, int channels);

    // decode into frames of 20ms
    QVector<QByteArray> decode(const QByteArray &opusData);

    QByteArray decodePlc();

private:
    QVector<QByteArray> splitFrames(const QByteArray &pcm, int totalSamples);

    ::OpusDecoder *decoder = nullptr;
    int frameSamples = 0;
    int frameChannels = 0;
    int frameBytes = 0;
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
