#include "Core/Audio/OpusEncoder.hpp"

#include <QTest>

#include <random>

namespace Audio = Acheron::Core::Audio;

namespace {

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 2;
constexpr int FRAME_SAMPLES = SAMPLE_RATE / 50;
constexpr int FRAMES = 100;

QByteArray noiseFrame(std::mt19937 &random)
{
    QByteArray frame(FRAME_SAMPLES * CHANNELS * static_cast<int>(sizeof(qint16)), '\0');
    auto *samples = reinterpret_cast<qint16 *>(frame.data());
    std::uniform_int_distribution<int> anySample(-30000, 30000);
    for (int i = 0; i < FRAME_SAMPLES * CHANNELS; i++)
        samples[i] = static_cast<qint16>(anySample(random));
    return frame;
}

int largestPacket(int bitrate)
{
    Audio::OpusEncoder encoder;
    if (!encoder.init(SAMPLE_RATE, CHANNELS))
        return -1;
    encoder.setBitrate(bitrate);

    std::mt19937 random(7);
    int largest = 0;
    for (int i = 0; i < FRAMES; i++) {
        const QByteArray packet = encoder.encode(noiseFrame(random));
        if (packet.isEmpty())
            return -1;
        largest = std::max(largest, static_cast<int>(packet.size()));
    }
    return largest;
}

} // namespace

class TestOpusEncoder : public QObject
{
    Q_OBJECT
private slots:
    void theHighestBitrateStillFitsOnePacket();
    void anOrdinaryBitrateIsNotHeldBack();
};

void TestOpusEncoder::theHighestBitrateStillFitsOnePacket()
{
    const int largest = largestPacket(510000);
    QVERIFY2(largest > 0, "encoding failed");
    QVERIFY2(largest <= Audio::OpusEncoder::MAX_PACKET_BYTES, qPrintable(QString::number(largest)));
    QVERIFY2(largest > Audio::OpusEncoder::MAX_PACKET_BYTES * 9 / 10, qPrintable(QString::number(largest)));
}

void TestOpusEncoder::anOrdinaryBitrateIsNotHeldBack()
{
    // 128 kbps is 320 bytes per 20 ms
    const int largest = largestPacket(128000);
    QVERIFY2(largest > 250 && largest < 500, qPrintable(QString::number(largest)));
}

QTEST_GUILESS_MAIN(TestOpusEncoder)
#include "tst_OpusEncoder.moc"
