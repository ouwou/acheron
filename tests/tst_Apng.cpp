// Tests for Core::Apng::Reader, the ffmpeg-backed animated-PNG reader behind APNG stickers.
// Fixtures are assembled here from PNGs Qt encodes, so every expectation is a pixel the APNG
// spec dictates (region placement, blend op, dispose op, delay fraction) and what is under test
// is our glue: in-memory IO, forcing the apng demuxer, frame/delay pairing, pixel format.

#include "Core/ApngDecoder.hpp"

#include <QBuffer>
#include <QTest>
#include <QtEndian>

using namespace Acheron::Core;

namespace {

constexpr int SignatureSize = 8;

enum DisposeOp : quint8 {
    DisposeNone = 0,
    DisposeBackground = 1,
    DisposePrevious = 2,
};

enum BlendOp : quint8 {
    BlendSource = 0,
    BlendOver = 1,
};

struct FrameSpec
{
    QImage image;
    QPoint offset;
    quint16 delayNumerator = 1;
    quint16 delayDenominator = 10;
    quint8 disposeOp = DisposeNone;
    quint8 blendOp = BlendSource;
};

quint32 crc32(const QByteArray &bytes)
{
    quint32 crc = 0xffffffffu;
    for (const char byte : bytes) {
        crc ^= quint8(byte);
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1) ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
    }
    return crc ^ 0xffffffffu;
}

QByteArray bigEndian32(quint32 value)
{
    QByteArray out(4, '\0');
    qToBigEndian<quint32>(value, out.data());
    return out;
}

QByteArray bigEndian16(quint16 value)
{
    QByteArray out(2, '\0');
    qToBigEndian<quint16>(value, out.data());
    return out;
}

QByteArray chunk(const QByteArray &type, const QByteArray &data)
{
    return bigEndian32(quint32(data.size())) + type + data + bigEndian32(crc32(type + data));
}

QByteArray encodePng(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QByteArray chunkData(const QByteArray &png, const QByteArray &wantedType)
{
    QByteArray out;
    qsizetype pos = SignatureSize;
    while (pos + 12 <= png.size()) {
        const quint32 length = qFromBigEndian<quint32>(png.constData() + pos);
        if (png.mid(pos + 4, 4) == wantedType)
            out += png.mid(pos + 8, length);
        pos += 12 + qsizetype(length);
    }
    return out;
}

QByteArray frameControl(quint32 sequence, const FrameSpec &frame)
{
    QByteArray data = bigEndian32(sequence) + bigEndian32(quint32(frame.image.width())) +
                      bigEndian32(quint32(frame.image.height())) + bigEndian32(quint32(frame.offset.x())) +
                      bigEndian32(quint32(frame.offset.y())) + bigEndian16(frame.delayNumerator) +
                      bigEndian16(frame.delayDenominator);
    data.append(char(frame.disposeOp));
    data.append(char(frame.blendOp));
    return data;
}

// the first frame doubles as the default image, so it has to span the canvas
QByteArray buildApng(const QList<FrameSpec> &frames)
{
    const QByteArray first = encodePng(frames.first().image);

    QByteArray out = first.left(SignatureSize);
    out += chunk("IHDR", chunkData(first, "IHDR"));
    out += chunk("acTL", bigEndian32(quint32(frames.size())) + bigEndian32(0));

    quint32 sequence = 0;
    for (qsizetype i = 0; i < frames.size(); ++i) {
        out += chunk("fcTL", frameControl(sequence++, frames[i]));
        const QByteArray imageData = chunkData(encodePng(frames[i].image), "IDAT");
        if (i == 0)
            out += chunk("IDAT", imageData);
        else
            out += chunk("fdAT", bigEndian32(sequence++) + imageData);
    }

    out += chunk("IEND", {});
    return out;
}

QImage solid(const QSize &size, const QColor &color)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}

struct DecodedFrame
{
    QImage canvas;
    int delayMs;
};

QList<DecodedFrame> decodeAll(const QByteArray &apng)
{
    QList<DecodedFrame> frames;
    const auto reader = Apng::Reader::open(apng);
    if (!reader)
        return frames;

    QImage canvas;
    int delayMs = 0;
    while (reader->next(canvas, delayMs))
        frames.append({ canvas.copy(), delayMs });
    return frames;
}

} // namespace

class TestApng : public QObject
{
    Q_OBJECT
private slots:
    void testPlainPngIsNotAnimated();
    void testReportsCanvasAndFrameCount();
    void testRegionIsPlacedAtOffset();
    void testBlendOverKeepsWhatIsUnderneath();
    void testBlendSourceReplacesWhatIsUnderneath();
    void testDisposeBackgroundClearsRegion();
    void testDisposePreviousRestoresRegion();
    void testDelayFraction();
    void testZeroDenominatorMeansHundredths();
    void testRegionOutsideCanvasYieldsNoAnimation();
};

void TestApng::testPlainPngIsNotAnimated()
{
    QVERIFY(!Apng::Reader::open(encodePng(solid({ 4, 4 }, Qt::red))));
    QVERIFY(!Apng::Reader::open(QByteArray("{\"v\":\"5.5.2\"}")));
}

void TestApng::testReportsCanvasAndFrameCount()
{
    const QByteArray apng = buildApng({ { solid({ 6, 4 }, Qt::red) }, { solid({ 6, 4 }, Qt::blue) }, { solid({ 6, 4 }, Qt::green) } });
    const auto reader = Apng::Reader::open(apng);
    QVERIFY(reader != nullptr);
    QCOMPARE(reader->canvasSize(), QSize(6, 4));
    QCOMPARE(reader->frameCount(), 3);
}

void TestApng::testRegionIsPlacedAtOffset()
{
    FrameSpec patch{ solid({ 2, 2 }, Qt::blue), QPoint(1, 2) };
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, Qt::red) }, patch }));

    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames[0].canvas.pixelColor(1, 2), QColor(Qt::red));
    QCOMPARE(frames[1].canvas.pixelColor(1, 2), QColor(Qt::blue));
    QCOMPARE(frames[1].canvas.pixelColor(2, 3), QColor(Qt::blue));
    QCOMPARE(frames[1].canvas.pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(frames[1].canvas.pixelColor(3, 3), QColor(Qt::red));
}

void TestApng::testBlendOverKeepsWhatIsUnderneath()
{
    FrameSpec overlay{ solid({ 4, 4 }, Qt::transparent) };
    overlay.image.setPixelColor(0, 0, Qt::blue);
    overlay.blendOp = BlendOver;
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, Qt::red) }, overlay }));

    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames[1].canvas.pixelColor(0, 0), QColor(Qt::blue));
    QCOMPARE(frames[1].canvas.pixelColor(1, 1), QColor(Qt::red));
}

void TestApng::testBlendSourceReplacesWhatIsUnderneath()
{
    FrameSpec overlay{ solid({ 4, 4 }, Qt::transparent) };
    overlay.image.setPixelColor(0, 0, Qt::blue);
    overlay.blendOp = BlendSource;
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, Qt::red) }, overlay }));

    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames[1].canvas.pixelColor(0, 0), QColor(Qt::blue));
    QCOMPARE(frames[1].canvas.pixelColor(1, 1).alpha(), 0);
}

void TestApng::testDisposeBackgroundClearsRegion()
{
    FrameSpec patch{ solid({ 2, 2 }, Qt::blue), QPoint(0, 0) };
    patch.disposeOp = DisposeBackground;
    FrameSpec dot{ solid({ 1, 1 }, Qt::green), QPoint(3, 3) };
    dot.blendOp = BlendOver;
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, Qt::red) }, patch, dot }));

    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames[1].canvas.pixelColor(0, 0), QColor(Qt::blue));
    QCOMPARE(frames[2].canvas.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(frames[2].canvas.pixelColor(2, 2), QColor(Qt::red));
    QCOMPARE(frames[2].canvas.pixelColor(3, 3), QColor(Qt::green));
}

void TestApng::testDisposePreviousRestoresRegion()
{
    FrameSpec patch{ solid({ 2, 2 }, Qt::blue), QPoint(0, 0) };
    patch.disposeOp = DisposePrevious;
    FrameSpec dot{ solid({ 1, 1 }, Qt::green), QPoint(3, 3) };
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, Qt::red) }, patch, dot }));

    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames[1].canvas.pixelColor(0, 0), QColor(Qt::blue));
    QCOMPARE(frames[2].canvas.pixelColor(0, 0), QColor(Qt::red));
}

void TestApng::testDelayFraction()
{
    FrameSpec first{ solid({ 2, 2 }, Qt::red) };
    first.delayNumerator = 1;
    first.delayDenominator = 25;
    FrameSpec second{ solid({ 2, 2 }, Qt::blue) };
    second.delayNumerator = 3;
    second.delayDenominator = 2;
    const auto frames = decodeAll(buildApng({ first, second }));

    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames[0].delayMs, 40);
    QCOMPARE(frames[1].delayMs, 1500);
}

void TestApng::testZeroDenominatorMeansHundredths()
{
    FrameSpec first{ solid({ 2, 2 }, Qt::red) };
    first.delayNumerator = 7;
    first.delayDenominator = 0;
    const auto frames = decodeAll(buildApng({ first, { solid({ 2, 2 }, Qt::blue) } }));

    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames[0].delayMs, 70);
}

void TestApng::testRegionOutsideCanvasYieldsNoAnimation()
{
    FrameSpec patch{ solid({ 2, 2 }, Qt::blue), QPoint(3, 3) };
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, Qt::red) }, patch }));
    QVERIFY(frames.size() < 2);
}

QTEST_GUILESS_MAIN(TestApng)
#include "tst_Apng.moc"
