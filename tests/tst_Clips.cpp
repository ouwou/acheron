#include "Core/Media/ClipDecoder.hpp"
#include "Core/Media/ClipPlayer.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Acheron::Core::Media;

namespace {

constexpr auto GifSixFramesTenthSecondEach =
        "R0lGODlhCAAIAPf/MQAA/gD+AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAAD/ACH/C05FVFNDQVBFMi4wAwEAAAAh+QQECgAfACwA"
        "AAAACAAIAAAIDwAFCBxIsKDBgwgTKkwYEAAh+QQFCgD/ACwHAAcAAQABAAAIBAD/BQQAIfkEBQoA/wAsAAAAAAgACAAACA8AAwgcSLCgwYMIEypMGBAAIfkE"
        "BQoA/wAsBwAHAAEAAQAACAQA/wUEACH5BAUKAP8ALAAAAAAIAAgAAAgPAAEIHEiwoMGDCBMqTBgQACH5BAUKAP8ALAcABwABAAEAAAgEAP8FBAA7";

constexpr auto WebpThreeFramesFifthSecondEach =
        "UklGRrQAAABXRUJQVlA4WAoAAAACAAAABwAABwAAQU5JTQYAAAD/////AABBTk1GKAAAAAAAAAAAAAcAAAcAAMgAAAJWUDhMDwAAAC8HwAEABxD1j/4HIqL/"
        "AQBBTk1GKAAAAAAAAAAAAAcAAAcAAMgAAABWUDhMDwAAAC8HwAEAB9D/yP4HIqL/AQBBTk1GKAAAAAAAAAAAAAcAAAcAAMgAAABWUDhMDwAAAC8HwAEABxDR"
        "//4HIqL/AQA=";

constexpr auto Mp4SixFramesTenthSecondEach =
        "AAAAIGZ0eXBpc29tAAACAGlzb21pc28yYXZjMW1wNDEAAAMxbW9vdgAAAGxtdmhkAAAAAAAAAAAAAAAAAAAD6AAAAlgAAQAAAQAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAlt0cmFrAAAAXHRraGQAAAADAAAAAAAAAAAAAAAB"
        "AAAAAAAAAlgAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAACAAAAAgAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAA"
        "AAEAAAJYAAAAAAABAAAAAAHTbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAAAoAAAAGABVxAAAAAAALWhkbHIAAAAAAAAAAHZpZGUAAAAAAAAAAAAAAABWaWRl"
        "b0hhbmRsZXIAAAABfm1pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAAT5zdGJsAAAArnN0c2QA"
        "AAAAAAAAAQAAAJ5hdmMxAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAACAAIABIAAAASAAAAAAAAAABGUxhdmM2Mi4yOC4xMDIgbGlib3BlbmgyNjQAAAAAAAAA"
        "GP//AAAAJGF2Y0MBQsAU/+EADWdCwBSMaJTAQHhEI1ABAARozjyAAAAAEHBhc3AAAAABAAAAAQAAABRidHJ0AAAAAAAABiUAAAYlAAAAGHN0dHMAAAAAAAAA"
        "AQAAAAYAAAQAAAAAFHN0c3MAAAAAAAAAAQAAAAEAAAAcc3RzYwAAAAAAAAABAAAAAQAAAAYAAAABAAAALHN0c3oAAAAAAAAAAAAAAAYAAAAbAAAACwAAAB0A"
        "AAALAAAAHQAAAAsAAAAUc3RjbwAAAAAAAAABAAADYQAAAGJ1ZHRhAAAAWm1ldGEAAAAAAAAAIWhkbHIAAAAAAAAAAG1kaXJhcHBsAAAAAAAAAAAAAAAALWls"
        "c3QAAAAlqXRvbwAAAB1kYXRhAAAAAQAAAABMYXZmNjIuMTIuMTAyAAAACGZyZWUAAAB+bWRhdAAAABdluAAECeIxQABCbjgACBXHAAEL6TqSvAAAAAdh4AB+"
        "QJ5YAAAAGWHgAL5A/xuKAAIIEcAAQtY4AAh44nzyZ/AAAAAHYeAA/kBPlgAAABlh4AE+QGfG4oAAmbxwABIEjgACBvifPJn8AAAAB2HgAX5Ad5Y=";

struct DecodedFrame
{
    QImage image;
    qint64 startMs;
};

QList<DecodedFrame> decodeLoop(ClipDecoder &decoder, const QSize &frameSize)
{
    QList<DecodedFrame> frames;
    QImage buffer;
    qint64 startMs = 0;
    while (decoder.nextFrameOfLoop(buffer, startMs, frameSize))
        frames.append({ buffer.copy(), startMs });
    return frames;
}

QList<qint64> startsOf(const QList<DecodedFrame> &frames)
{
    QList<qint64> starts;
    for (const DecodedFrame &frame : frames)
        starts.append(frame.startMs);
    return starts;
}

enum class Dominant {
    Red,
    Green,
    Blue,
    None,
};

Dominant dominantChannel(const QImage &image)
{
    if (image.isNull())
        return Dominant::None;

    const QColor color = image.pixelColor(image.width() / 2, image.height() / 2);
    if (color.red() > 180 && color.green() < 80 && color.blue() < 80)
        return Dominant::Red;
    if (color.green() > 180 && color.red() < 80 && color.blue() < 80)
        return Dominant::Green;
    if (color.blue() > 180 && color.red() < 80 && color.green() < 80)
        return Dominant::Blue;
    return Dominant::None;
}

} // namespace

class TestClips : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();

    void testGifFramesAndTiming();
    void testGifLoopsAfterRewind();
    void testGifReusesTheCallersBuffer();
    void testMp4FramesAndTiming();
    void testMp4LoopsAfterRewind();
    void testFramesAreScaledToTheRequestedSize();
    void testAnimatedWebpFramesAndTiming();
    void testAnimatedWebpReusesTheCallersBuffer();
    void testFilesThatAreNotClipsDoNotOpen();

    void testPlayerShowsNothingUntilPlaying();
    void testPlayerPacesFramesAndLoops();
    void testPlayerStopsWhilePaused();
    void testPlayerStopsAfterClose();
    void testPlayerReportsUnreadableClip();
    void testPlayerReportsSingleFrameImageAsFailed();
    void testPlayerNeverUpscales();
    void testPlayerRefusesClipOverItsMemoryLimit();

private:
    [[nodiscard]] QString fixture(const char *name, const char *base64) const;
    [[nodiscard]] static bool qtReadsWebp() { return QImageReader::supportedImageFormats().contains("webp"); }

    QTemporaryDir dir;
    QString gif;
    QString webp;
    QString mp4;
};

QString TestClips::fixture(const char *name, const char *base64) const
{
    const QString path = dir.filePath(QString::fromLatin1(name));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(QByteArray::fromBase64(base64));
    return path;
}

void TestClips::initTestCase()
{
    QVERIFY(dir.isValid());
    gif = fixture("six.gif", GifSixFramesTenthSecondEach);
    webp = fixture("three.webp", WebpThreeFramesFifthSecondEach);
    mp4 = fixture("six.mp4", Mp4SixFramesTenthSecondEach);
}

void TestClips::testGifFramesAndTiming()
{
    const auto decoder = ClipDecoder::open(gif);
    QVERIFY(decoder != nullptr);
    QCOMPARE(decoder->nativeSize(), QSize(8, 8));

    const auto frames = decodeLoop(*decoder, QSize(8, 8));
    QCOMPARE(startsOf(frames), (QList<qint64>{ 0, 100, 200, 300, 400, 500 }));
    QCOMPARE(decoder->finishedLoopMs(), qint64(600));

    QCOMPARE(dominantChannel(frames[0].image), Dominant::Red);
    QCOMPARE(dominantChannel(frames[1].image), Dominant::Red);
    QCOMPARE(dominantChannel(frames[2].image), Dominant::Green);
    QCOMPARE(dominantChannel(frames[3].image), Dominant::Green);
    QCOMPARE(dominantChannel(frames[4].image), Dominant::Blue);
    QCOMPARE(dominantChannel(frames[5].image), Dominant::Blue);
    QCOMPARE(frames[0].image.format(), QImage::Format_ARGB32_Premultiplied);
}

void TestClips::testGifLoopsAfterRewind()
{
    const auto decoder = ClipDecoder::open(gif);
    QVERIFY(decoder != nullptr);
    QCOMPARE(decodeLoop(*decoder, QSize(8, 8)).size(), 6);

    for (int loop = 0; loop < 3; ++loop) {
        QVERIFY(decoder->rewind());
        const auto frames = decodeLoop(*decoder, QSize(8, 8));
        QCOMPARE(startsOf(frames), (QList<qint64>{ 0, 100, 200, 300, 400, 500 }));
        QCOMPARE(dominantChannel(frames[0].image), Dominant::Red);
        QCOMPARE(dominantChannel(frames[5].image), Dominant::Blue);
        QCOMPARE(decoder->finishedLoopMs(), qint64(600));
    }
}

void TestClips::testGifReusesTheCallersBuffer()
{
    const auto decoder = ClipDecoder::open(gif);
    QVERIFY(decoder != nullptr);

    QImage buffer;
    qint64 startMs = 0;
    QVERIFY(decoder->nextFrameOfLoop(buffer, startMs, QSize(8, 8)));
    const uchar *firstPixels = buffer.constBits();
    QVERIFY(decoder->nextFrameOfLoop(buffer, startMs, QSize(8, 8)));
    QCOMPARE(buffer.constBits(), firstPixels);

    const QImage heldElsewhere = buffer;
    QVERIFY(decoder->nextFrameOfLoop(buffer, startMs, QSize(8, 8)));
    QVERIFY(buffer.constBits() != heldElsewhere.constBits());
    QCOMPARE(dominantChannel(heldElsewhere), Dominant::Red);
    QCOMPARE(dominantChannel(buffer), Dominant::Green);
}

void TestClips::testMp4FramesAndTiming()
{
    const auto decoder = ClipDecoder::open(mp4);
    QVERIFY(decoder != nullptr);
    QCOMPARE(decoder->nativeSize(), QSize(32, 32));

    const auto frames = decodeLoop(*decoder, QSize(32, 32));
    QCOMPARE(startsOf(frames), (QList<qint64>{ 0, 100, 200, 300, 400, 500 }));
    QCOMPARE(decoder->finishedLoopMs(), qint64(600));

    QCOMPARE(dominantChannel(frames[0].image), Dominant::Red);
    QCOMPARE(dominantChannel(frames[2].image), Dominant::Green);
    QCOMPARE(dominantChannel(frames[5].image), Dominant::Blue);
    QCOMPARE(frames[0].image.format(), QImage::Format_RGB32);
}

void TestClips::testMp4LoopsAfterRewind()
{
    const auto decoder = ClipDecoder::open(mp4);
    QVERIFY(decoder != nullptr);
    QCOMPARE(decodeLoop(*decoder, QSize(32, 32)).size(), 6);

    for (int loop = 0; loop < 3; ++loop) {
        QVERIFY(decoder->rewind());
        const auto frames = decodeLoop(*decoder, QSize(32, 32));
        QCOMPARE(startsOf(frames), (QList<qint64>{ 0, 100, 200, 300, 400, 500 }));
        QCOMPARE(dominantChannel(frames[0].image), Dominant::Red);
        QCOMPARE(dominantChannel(frames[5].image), Dominant::Blue);
    }
}

void TestClips::testFramesAreScaledToTheRequestedSize()
{
    const auto decoder = ClipDecoder::open(mp4);
    QVERIFY(decoder != nullptr);

    const auto frames = decodeLoop(*decoder, QSize(16, 12));
    QCOMPARE(frames.size(), 6);
    QCOMPARE(frames[0].image.size(), QSize(16, 12));
    QCOMPARE(dominantChannel(frames[0].image), Dominant::Red);
    QCOMPARE(dominantChannel(frames[5].image), Dominant::Blue);
}

void TestClips::testAnimatedWebpFramesAndTiming()
{
    if (!qtReadsWebp())
        QSKIP("this Qt has no WebP image plugin");

    const auto decoder = ClipDecoder::open(webp);
    QVERIFY(decoder != nullptr);
    QCOMPARE(decoder->nativeSize(), QSize(8, 8));

    for (int loop = 0; loop < 2; ++loop) {
        const auto frames = decodeLoop(*decoder, QSize(8, 8));
        QCOMPARE(startsOf(frames), (QList<qint64>{ 0, 200, 400 }));
        QCOMPARE(decoder->finishedLoopMs(), qint64(600));
        QCOMPARE(dominantChannel(frames[0].image), Dominant::Red);
        QCOMPARE(dominantChannel(frames[1].image), Dominant::Green);
        QCOMPARE(dominantChannel(frames[2].image), Dominant::Blue);
        QCOMPARE(frames[0].image.format(), QImage::Format_ARGB32_Premultiplied);
        QVERIFY(decoder->rewind());
    }
}

void TestClips::testAnimatedWebpReusesTheCallersBuffer()
{
    if (!qtReadsWebp())
        QSKIP("this Qt has no WebP image plugin");

    const auto decoder = ClipDecoder::open(webp);
    QVERIFY(decoder != nullptr);

    QImage buffer;
    qint64 startMs = 0;
    QVERIFY(decoder->nextFrameOfLoop(buffer, startMs, QSize(8, 8)));
    const uchar *firstPixels = buffer.constBits();
    QCOMPARE(dominantChannel(buffer), Dominant::Red);
    QVERIFY(decoder->nextFrameOfLoop(buffer, startMs, QSize(8, 8)));
    QCOMPARE(buffer.constBits(), firstPixels);
    QCOMPARE(dominantChannel(buffer), Dominant::Green);

    const QImage heldElsewhere = buffer;
    QVERIFY(decoder->nextFrameOfLoop(buffer, startMs, QSize(8, 8)));
    QVERIFY(buffer.constBits() != heldElsewhere.constBits());
    QCOMPARE(dominantChannel(heldElsewhere), Dominant::Green);
    QCOMPARE(dominantChannel(buffer), Dominant::Blue);
}

void TestClips::testFilesThatAreNotClipsDoNotOpen()
{
    QVERIFY(!ClipDecoder::open(dir.filePath("missing.gif")));

    const char *hlsPlaylistNamedGif = "I0VYVE0zVQpodHRwOi8vZXhhbXBsZS5jb20vYS50cwo=";
    QVERIFY(!ClipDecoder::open(fixture("playlist.gif", hlsPlaylistNamedGif)));

    QImage still(4, 4, QImage::Format_ARGB32);
    still.fill(Qt::red);
    const QString png = dir.filePath("still.png");
    QVERIFY(still.save(png, "PNG"));
    QVERIFY(!ClipDecoder::open(png));

    const char *gifHeaderOnly = "R0lGODlhCAAIAPf/MQ==";
    const auto truncated = ClipDecoder::open(fixture("truncated.gif", gifHeaderOnly));
    QVERIFY(!truncated || decodeLoop(*truncated, QSize(8, 8)).isEmpty());
}

void TestClips::testPlayerShowsNothingUntilPlaying()
{
    ClipPlayer player;
    QSignalSpy changed(&player, &ClipPlayer::frameChanged);
    const auto id = player.open(gif, QSize(8, 8), Qt::KeepAspectRatio);

    QTest::qWait(150);
    QCOMPARE(changed.count(), 0);
    QVERIFY(player.frame(id).isNull());
}

void TestClips::testPlayerPacesFramesAndLoops()
{
    ClipPlayer player;
    QSignalSpy changed(&player, &ClipPlayer::frameChanged);
    const auto id = player.open(gif, QSize(8, 8), Qt::KeepAspectRatio);
    player.setPlaying(id, true);

    QVERIFY(changed.wait(2000));
    QCOMPARE(changed.first().first().toULongLong(), quint64(id));
    QCOMPARE(dominantChannel(player.frame(id)), Dominant::Red);

    QElapsedTimer sinceFirstFrame;
    sinceFirstFrame.start();

    QList<Dominant> seen;
    while (changed.count() < 9 && sinceFirstFrame.elapsed() < 5000) {
        changed.wait(200);
        seen.append(dominantChannel(player.frame(id)));
    }

    QVERIFY(changed.count() >= 9);
    QVERIFY2(sinceFirstFrame.elapsed() >= 650, "eight more frames at 100 ms each cannot arrive this early");
    QVERIFY2(sinceFirstFrame.elapsed() < 2500, "playback ran far slower than the clip's own timing");
    QVERIFY(seen.contains(Dominant::Green));
    QVERIFY(seen.contains(Dominant::Blue));
    QVERIFY(!seen.contains(Dominant::None));
}

void TestClips::testPlayerStopsWhilePaused()
{
    ClipPlayer player;
    QSignalSpy changed(&player, &ClipPlayer::frameChanged);
    const auto id = player.open(mp4, QSize(32, 32), Qt::KeepAspectRatio);
    player.setPlaying(id, true);
    QVERIFY(changed.wait(2000));

    player.setPlaying(id, false);
    const QImage frozen = player.frame(id);
    changed.clear();
    QTest::qWait(400);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(player.frame(id), frozen);

    player.setPlaying(id, true);
    QVERIFY(changed.wait(2000));
}

void TestClips::testPlayerStopsAfterClose()
{
    ClipPlayer player;
    QSignalSpy changed(&player, &ClipPlayer::frameChanged);
    const auto id = player.open(gif, QSize(8, 8), Qt::KeepAspectRatio);
    player.setPlaying(id, true);
    QVERIFY(changed.wait(2000));

    player.close(id);
    changed.clear();
    QTest::qWait(400);
    QCOMPARE(changed.count(), 0);
    QVERIFY(player.frame(id).isNull());
}

void TestClips::testPlayerReportsUnreadableClip()
{
    ClipPlayer player;
    QSignalSpy failed(&player, &ClipPlayer::clipFailed);
    const auto id = player.open(dir.filePath("missing.gif"), QSize(8, 8), Qt::KeepAspectRatio);
    player.setPlaying(id, true);

    QVERIFY(failed.wait(2000));
    QCOMPARE(failed.first().first().toULongLong(), quint64(id));

    QTest::qWait(200);
    QCOMPARE(failed.count(), 1);
}

void TestClips::testPlayerReportsSingleFrameImageAsFailed()
{
    if (!qtReadsWebp() || !QImageWriter::supportedImageFormats().contains("webp"))
        QSKIP("this Qt cannot write a WebP fixture");

    QImage still(8, 8, QImage::Format_ARGB32);
    still.fill(Qt::red);
    const QString path = dir.filePath("still.webp");
    QVERIFY(still.save(path, "WEBP"));

    ClipPlayer player;
    QSignalSpy failed(&player, &ClipPlayer::clipFailed);
    const auto id = player.open(path, QSize(8, 8), Qt::KeepAspectRatio);
    player.setPlaying(id, true);

    QVERIFY(failed.wait(2000));
    QCOMPARE(failed.first().first().toULongLong(), quint64(id));
}

void TestClips::testPlayerNeverUpscales()
{
    ClipPlayer player;

    const auto roomy = player.open(mp4, QSize(400, 300), Qt::KeepAspectRatio);
    player.setPlaying(roomy, true);
    QTRY_VERIFY_WITH_TIMEOUT(!player.frame(roomy).isNull(), 2000);
    QCOMPARE(player.frame(roomy).size(), QSize(32, 32));

    const auto fitted = player.open(mp4, QSize(16, 8), Qt::KeepAspectRatio);
    player.setPlaying(fitted, true);
    QTRY_VERIFY_WITH_TIMEOUT(!player.frame(fitted).isNull(), 2000);
    QCOMPARE(player.frame(fitted).size(), QSize(8, 8));

    const auto covering = player.open(mp4, QSize(16, 8), Qt::KeepAspectRatioByExpanding);
    player.setPlaying(covering, true);
    QTRY_VERIFY_WITH_TIMEOUT(!player.frame(covering).isNull(), 2000);
    QCOMPARE(player.frame(covering).size(), QSize(16, 16));
}

void TestClips::testPlayerRefusesClipOverItsMemoryLimit()
{
    const QSize mp4Size(32, 32);
    const qint64 needed = ClipPlayer::memoryToPlay(mp4Size, mp4Size);

    ClipPlayer player;
    QSignalSpy failed(&player, &ClipPlayer::clipFailed);

    player.setMemoryLimitForNewClips(needed - 1);
    const auto refused = player.open(mp4, QSize(400, 300), Qt::KeepAspectRatio);
    player.setPlaying(refused, true);
    QVERIFY(failed.wait(2000));
    QCOMPARE(failed.first().first().toULongLong(), quint64(refused));
    QVERIFY(player.frame(refused).isNull());

    player.setMemoryLimitForNewClips(needed);
    const auto allowed = player.open(mp4, QSize(400, 300), Qt::KeepAspectRatio);
    player.setPlaying(allowed, true);
    QTRY_VERIFY_WITH_TIMEOUT(!player.frame(allowed).isNull(), 2000);
    QCOMPARE(failed.count(), 1);
}

QTEST_GUILESS_MAIN(TestClips)
#include "tst_Clips.moc"
