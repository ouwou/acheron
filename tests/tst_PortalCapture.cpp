#include <QtTest>

#include "Core/Video/LinuxCapture.hpp"

#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)

#include "PipeWireTestSource.hpp"
#include "ProcessUsage.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDBusVirtualObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>

#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/macros.h>
}

using namespace Acheron::Core::Video;
using Result = LinuxGrabber::Result;
using ProcessUsage::threadsOfThisProcess;

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

const char NO_CARD_MEMORY[] = "This machine does not let a program make graphics card buffers of its own (/dev/udmabuf), so there is nothing to hand over as one.";
constexpr QSize SMALL(64, 48);
constexpr QSize LARGER(96, 80);
constexpr int SOURCE_BUFFERS = PipeWireTestSource::BUFFERS;

struct FrameDeleter
{
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
using FramePointer = std::unique_ptr<AVFrame, FrameDeleter>;

FramePointer emptyFrame()
{
    return FramePointer(av_frame_alloc());
}

quint32 pixelOf(const QSize &size, int x, int y, quint32 number)
{
    if (x < 4 && y < 4)
        return 0xff000000u | number;
    const bool right = x >= size.width() / 2;
    const bool lower = y >= size.height() / 2;
    return lower ? (right ? 0xffa0b0c0u : 0xff708090u) : (right ? 0xff405060u : 0xff102030u);
}

void draw(void *into, const QSize &size, int stride, quint32 number, bool)
{
    for (int y = 0; y < size.height(); y++) {
        auto *row = reinterpret_cast<quint32 *>(static_cast<uint8_t *>(into) + static_cast<qsizetype>(y) * stride);
        for (int x = 0; x < size.width(); x++)
            row[x] = pixelOf(size, x, y, number);
    }
}

int pixelsThatDiffer(const uint8_t *data, int stride, const QSize &size, quint32 number)
{
    int differing = 0;
    for (int y = 0; y < size.height(); y++) {
        const auto *row = reinterpret_cast<const quint32 *>(data + static_cast<qsizetype>(y) * stride);
        for (int x = 0; x < size.width(); x++)
            differing += row[x] != pixelOf(size, x, y, number);
    }
    return differing;
}

int pixelsThatDiffer(const AVFrame *frame, quint32 number)
{
    return pixelsThatDiffer(frame->data[0], frame->linesize[0], QSize(frame->width, frame->height), number);
}

const AVDRMFrameDescriptor *layoutOf(const AVFrame *onCard)
{
    return reinterpret_cast<const AVDRMFrameDescriptor *>(onCard->data[0]);
}

int pixelsThatDifferOnCard(const AVFrame *onCard, quint32 number)
{
    const AVDRMFrameDescriptor *layout = layoutOf(onCard);
    void *mapped = mmap(nullptr, layout->objects[0].size, PROT_READ, MAP_SHARED, layout->objects[0].fd, 0);
    if (mapped == MAP_FAILED)
        return -1;
    const AVDRMPlaneDescriptor &plane = layout->layers[0].planes[0];
    const int differing = pixelsThatDiffer(static_cast<const uint8_t *>(mapped) + plane.offset, static_cast<int>(plane.pitch), QSize(onCard->width, onCard->height), number);
    munmap(mapped, layout->objects[0].size);
    return differing;
}

quint32 numberOf(const AVFrame *frame)
{
    if (frame->format != AV_PIX_FMT_DRM_PRIME)
        return *reinterpret_cast<const quint32 *>(frame->data[0]) & 0x00ffffffu;

    const AVDRMFrameDescriptor *layout = layoutOf(frame);
    void *mapped = mmap(nullptr, layout->objects[0].size, PROT_READ, MAP_SHARED, layout->objects[0].fd, 0);
    if (mapped == MAP_FAILED)
        return 0;
    const quint32 number = *static_cast<const quint32 *>(mapped) & 0x00ffffffu;
    munmap(mapped, layout->objects[0].size);
    return number;
}

std::optional<size_t> hashOnCard(const AVFrame *onCard)
{
    const AVDRMFrameDescriptor *layout = layoutOf(onCard);
    void *mapped = mmap(nullptr, layout->objects[0].size, PROT_READ, MAP_SHARED, layout->objects[0].fd, 0);
    if (mapped == MAP_FAILED)
        return std::nullopt;
    const size_t hash = qHashBits(mapped, layout->objects[0].size);
    munmap(mapped, layout->objects[0].size);
    return hash;
}

QElapsedTimer startedTimer()
{
    QElapsedTimer timer;
    timer.start();
    return timer;
}

int descriptorsOfThisProcess()
{
    return static_cast<int>(QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot).size());
}

std::optional<int> pipeWireObjectsOfThisProcess()
{
    QProcess dump;
    dump.start(QStringLiteral("pw-dump"), QStringList());
    if (!dump.waitForFinished(5000) || dump.exitCode() != 0)
        return std::nullopt;

    const QJsonArray objects = QJsonDocument::fromJson(dump.readAllStandardOutput()).array();
    QSet<int> clients;
    for (const QJsonValue &object : objects) {
        const QJsonObject properties = object[QLatin1String("info")][QLatin1String("props")].toObject();
        if (object[QLatin1String("type")].toString() == QLatin1String("PipeWire:Interface:Client") && properties[QLatin1String("application.process.id")].toInt() == getpid())
            clients.insert(object[QLatin1String("id")].toInt());
    }
    int found = static_cast<int>(clients.size());
    for (const QJsonValue &object : objects) {
        const QJsonObject properties = object[QLatin1String("info")][QLatin1String("props")].toObject();
        found += object[QLatin1String("type")].toString() == QLatin1String("PipeWire:Interface:Node") && clients.contains(properties[QLatin1String("client.id")].toInt());
    }
    return found;
}

class TestSource : public PipeWireTestSource
{
public:
    explicit TestSource(const QSize &size, spa_video_format format = SPA_VIDEO_FORMAT_BGRx, bool offersCard = false) : PipeWireTestSource(size, draw, format, offersCard) {}
};

bool firstPictureArrives(PipeWireTestSource &source, LinuxGrabber *grabber, AVFrame *into, quint32 number = 0)
{
    const auto giveUp = Clock::now() + std::chrono::seconds(10);
    while (grabber && Clock::now() < giveUp) {
        source.send(number);
        if (grabber->grab(into, milliseconds(50)) == Result::Frame)
            return true;
    }
    return false;
}

int sessionsAtThePortal()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString path = QStringLiteral("/org/freedesktop/portal/desktop/session/") + bus.baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('_'));
    const QDBusMessage answer = bus.call(QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"), path, QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect")), QDBus::Block, 2000);
    return static_cast<int>(answer.arguments().value(0).toString().count(QLatin1String("<node name=")));
}

struct OfferedStream
{
    uint node = 0;
    QVariantMap properties;
};

struct OfferedSize
{
    int width = 0;
    int height = 0;
};

QDBusArgument &operator<<(QDBusArgument &argument, const OfferedStream &stream)
{
    argument.beginStructure();
    argument << stream.node << stream.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, OfferedStream &stream)
{
    argument.beginStructure();
    argument >> stream.node >> stream.properties;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const OfferedSize &size)
{
    argument.beginStructure();
    argument << size.width << size.height;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, OfferedSize &size)
{
    argument.beginStructure();
    argument >> size.width >> size.height;
    argument.endStructure();
    return argument;
}

} // namespace

Q_DECLARE_METATYPE(OfferedStream)
Q_DECLARE_METATYPE(OfferedSize)

namespace {

class PortalOfTheTests : public QDBusVirtualObject
{
public:
    enum class Step {
        None,
        CreateSession,
        SelectSources,
        Start,
        OpenPipeWireRemote,
    };

    struct Told
    {
        Step silentAt = Step::None;
        uint startResponse = 0;
        bool namesItsRequestsItself = false;
        int msBeforeItsProperties = 0;
        quint32 node = 0;
        QSize size;
        QString nextToken;
    };

    struct Asked
    {
        QStringList calls;
        QVariantMap sourceOptions;
        int forItsProperties = 0;
    };

    PortalOfTheTests()
    {
        qDBusRegisterMetaType<OfferedStream>();
        qDBusRegisterMetaType<QList<OfferedStream>>();
        qDBusRegisterMetaType<OfferedSize>();
        bus().registerVirtualObject(QStringLiteral("/org/freedesktop/portal/desktop"), this, QDBusConnection::SubPath);
        moveToThread(&thread);
        thread.start();
    }

    ~PortalOfTheTests() override
    {
        thread.quit();
        thread.wait();
        QDBusConnection::disconnectFromBus(QStringLiteral("portal-of-the-tests"));
    }

    void appear() { bus().registerService(QStringLiteral("org.freedesktop.portal.Desktop")); }
    void disappear() { bus().unregisterService(QStringLiteral("org.freedesktop.portal.Desktop")); }

    void tell(const Told &what)
    {
        std::lock_guard<std::mutex> lock(guard);
        told = what;
        asked = Asked();
    }

    [[nodiscard]] Asked whatWasAsked()
    {
        std::lock_guard<std::mutex> lock(guard);
        return asked;
    }

    void answerStart(uint response)
    {
        std::lock_guard<std::mutex> lock(guard);
        QDBusMessage answer = QDBusMessage::createSignal(startRequest, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"));
        answer << response << (response == 0 ? started() : QVariantMap());
        bus().send(answer);
    }

    void closeTheSession()
    {
        std::lock_guard<std::mutex> lock(guard);
        QDBusMessage closed = QDBusMessage::createSignal(session, QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Closed"));
        closed << QVariantMap();
        bus().send(closed);
    }

    QString introspect(const QString &) const override { return QString(); }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        std::unique_lock<std::mutex> lock(guard);
        const QString member = message.member();
        if (message.interface() == QLatin1String("org.freedesktop.DBus.Properties") && member == QLatin1String("GetAll")) {
            asked.forItsProperties++;
            const int wait = told.msBeforeItsProperties;
            lock.unlock();
            QThread::msleep(static_cast<unsigned long>(wait));
            const QVariantMap properties{ { QStringLiteral("version"), 5u }, { QStringLiteral("AvailableSourceTypes"), 3u }, { QStringLiteral("AvailableCursorModes"), 2u } };
            return connection.send(message.createReply(QVariant(properties)));
        }
        if (message.interface() == QLatin1String("org.freedesktop.portal.Session") && member == QLatin1String("Close")) {
            asked.calls.append(QStringLiteral("Session.Close"));
            return connection.send(message.createReply());
        }
        if (message.interface() != QLatin1String("org.freedesktop.portal.ScreenCast"))
            return false;

        asked.calls.append(member);
        if (member == QLatin1String("OpenPipeWireRemote")) {
            if (told.silentAt == Step::OpenPipeWireRemote)
                return true;
            const int remote = connectedToPipeWire();
            const bool sent = connection.send(message.createReply(QVariant::fromValue(QDBusUnixFileDescriptor(remote))));
            ::close(remote);
            return sent;
        }

        const QVariantMap options = qdbus_cast<QVariantMap>(message.arguments().last());
        const QString sender = message.service().mid(1).replace(QLatin1Char('.'), QLatin1Char('_'));
        const QString request = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(told.namesItsRequestsItself ? QStringLiteral("named_by_the_portal") : sender, options.value(QStringLiteral("handle_token")).toString());
        connection.send(message.createReply(QVariant::fromValue(QDBusObjectPath(request))));
        if (member == QLatin1String("CreateSession")) {
            session = QStringLiteral("/org/freedesktop/portal/desktop/session/%1/%2").arg(sender, options.value(QStringLiteral("session_handle_token")).toString());
            if (told.silentAt != Step::CreateSession)
                respond(request, 0, { { QStringLiteral("session_handle"), session } });
        } else if (member == QLatin1String("SelectSources")) {
            asked.sourceOptions = options;
            if (told.silentAt != Step::SelectSources)
                respond(request, 0, {});
        } else if (member == QLatin1String("Start")) {
            startRequest = request;
            if (told.silentAt != Step::Start)
                respond(request, told.startResponse, told.startResponse == 0 ? started() : QVariantMap());
        }
        return true;
    }

private:
    static QDBusConnection bus()
    {
        static const QDBusConnection connection = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("portal-of-the-tests"));
        return connection;
    }

    static int connectedToPipeWire()
    {
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        const QByteArray path = qgetenv("XDG_RUNTIME_DIR") + "/pipewire-0";
        memcpy(address.sun_path, path.constData(), std::min<size_t>(static_cast<size_t>(path.size()), sizeof(address.sun_path) - 1));
        const int remote = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (::connect(remote, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
            qWarning("the portal of the tests could not reach PipeWire");
        return remote;
    }

    QVariantMap started() const
    {
        const QVariantMap properties{ { QStringLiteral("size"), QVariant::fromValue(OfferedSize{ told.size.width(), told.size.height() }) }, { QStringLiteral("source_type"), 1u } };
        return { { QStringLiteral("streams"), QVariant::fromValue(QList<OfferedStream>{ OfferedStream{ told.node, properties } }) }, { QStringLiteral("restore_token"), told.nextToken } };
    }

    // a portal that names its requests itself cant be listened to before it is asked, so it gives the
    // caller time to find out where to listen
    void respond(const QString &request, uint response, const QVariantMap &results)
    {
        QTimer::singleShot(told.namesItsRequestsItself ? 200 : 0, this, [request, response, results] {
            QDBusMessage answer = QDBusMessage::createSignal(request, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"));
            answer << response << results;
            bus().send(answer);
        });
    }

    QThread thread;
    std::mutex guard;
    Told told;
    Asked asked;
    QString session;
    QString startRequest;
};

struct Capture
{
    explicit Capture(const QSize &size, int framerate = 1000, bool onCard = false, spa_video_format format = SPA_VIDEO_FORMAT_BGRx)
        : source(std::make_unique<TestSource>(size, format, onCard))
    {
        const quint32 node = source->node();
        if (node == PW_ID_ANY)
            return;
        adopted = PortalCapture::adopt(node, size);
        grabber = PortalCapture::open(adopted.id, framerate, onCard);
        running = firstPictureArrives(*source, grabber.get(), frame.get());
        av_frame_unref(frame.get());
    }

    ~Capture()
    {
        grabber.reset();
        PortalCapture::release(adopted.id);
    }

    Result grab(int waitMs = 500) { return grabber->grab(frame.get(), milliseconds(waitMs)); }

    std::unique_ptr<TestSource> source;
    CaptureSource adopted;
    std::unique_ptr<LinuxGrabber> grabber;
    FramePointer frame = emptyFrame();
    bool running = false;
};

} // namespace

class TestPortalCapture : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();

    void aPictureArrivesWithEveryPixelAsItWasSent_data();
    void aPictureArrivesWithEveryPixelAsItWasSent();
    void theNewestPictureWinsAndIsHandedOverOnce();
    void nothingNewIsNoFrameAfterTheWaitAndNoLater();
    void aSourceFasterThanTheFrameRateIsThinned();
    void aSpoiledPictureIsPassedOver_data();
    void aSpoiledPictureIsPassedOver();
    void aCutOutIsMarkedOnThePicture();
    void aChangeOfSizeBringsPicturesOfTheNewSize();
    void theSizeOfASourceIsWhatItsPicturesTurnOutToBe();
    void aPictureKeptStaysAsItWasWhileLaterOnesArrive();
    void endingTheSourceIsClosed();
    void releasingTheSourceClosesWhatCapturesIt();
    void aNodeThatIsNotThereFails();
    void aSourceNobodyRegisteredCannotBeOpened();
    void openingAndClosingTwentyTimesLeavesNothingBehind();

    void aPictureOnTheCardIsHandedOverWhereItIs();
    void syncObjectsAfterAPicturesPlanesAreNotTakenForPlanes();
    void aPictureOnTheCardIsNotDrawnOverWhileItIsKept();
    void keepingMorePicturesThanTheSourceCanSpareDoesNotStopIt();
    void aPictureOnTheCardOutlivesWhatCapturedIt();
    void memoryIsUsedWhereTheCardWasNotAskedFor();
    void cardBuffersAnotherConsumerSettledAreNotHandedOverAsMemory();

    void anAskingWhoseReceiverIsGoneAnswersNobodyAndLeavesNoSession();
    void anAskingThePortalTurnsDownEndsWithAReasonAndNoSession();
    void aPortalOfTheTestsOwnIsAskedOnABusOfTheTestsOwn();
    void theRealScreenIsCapturedInMemoryAndOnTheCard();
};

void TestPortalCapture::initTestCase()
{
    Capture capture(SMALL);
    if (!capture.running)
        QSKIP("PipeWire does not carry a picture from one stream of this process to another here, so there is nothing to capture.");
}

void TestPortalCapture::aPictureArrivesWithEveryPixelAsItWasSent_data()
{
    QTest::addColumn<int>("sent");
    QTest::addColumn<int>("handedOver");
    QTest::addColumn<QString>("named");

    QTest::newRow("BGRx") << int(SPA_VIDEO_FORMAT_BGRx) << int(AV_PIX_FMT_BGR0) << QStringLiteral("pipewire memfd BGRx");
    QTest::newRow("BGRA") << int(SPA_VIDEO_FORMAT_BGRA) << int(AV_PIX_FMT_BGRA) << QStringLiteral("pipewire memfd BGRA");
    QTest::newRow("RGBx") << int(SPA_VIDEO_FORMAT_RGBx) << int(AV_PIX_FMT_RGB0) << QStringLiteral("pipewire memfd RGBx");
    QTest::newRow("RGBA") << int(SPA_VIDEO_FORMAT_RGBA) << int(AV_PIX_FMT_RGBA) << QStringLiteral("pipewire memfd RGBA");
}

void TestPortalCapture::aPictureArrivesWithEveryPixelAsItWasSent()
{
    QFETCH(int, sent);
    QFETCH(int, handedOver);
    QFETCH(QString, named);

    Capture capture(SMALL, 1000, false, static_cast<spa_video_format>(sent));
    QVERIFY(capture.running);
    QVERIFY(capture.source->send(77));
    QCOMPARE(capture.grab(), Result::Frame);

    QCOMPARE(capture.frame->format, handedOver);
    QCOMPARE(QSize(capture.frame->width, capture.frame->height), SMALL);
    QCOMPARE(pixelsThatDiffer(capture.frame.get(), 77), 0);
    QCOMPARE(capture.grabber->name(), named);
    QVERIFY(capture.grabber->deliversOnlyChanges());
}

void TestPortalCapture::theNewestPictureWinsAndIsHandedOverOnce()
{
    Capture capture(SMALL);
    QVERIFY(capture.running);
    for (quint32 number = 1; number <= 3; number++) {
        QVERIFY(capture.source->send(number));
        QTest::qWait(30);
    }

    QCOMPARE(capture.grab(), Result::Frame);
    QCOMPARE(numberOf(capture.frame.get()), 3u);
    QCOMPARE(capture.grab(100), Result::NoFrame);
}

void TestPortalCapture::nothingNewIsNoFrameAfterTheWaitAndNoLater()
{
    Capture capture(SMALL);
    QVERIFY(capture.running);

    QElapsedTimer waited;
    waited.start();
    QCOMPARE(capture.grab(120), Result::NoFrame);
    QVERIFY2(waited.elapsed() >= 110 && waited.elapsed() < 200, qPrintable(QString::number(waited.elapsed())));
}

void TestPortalCapture::aSourceFasterThanTheFrameRateIsThinned()
{
    constexpr int FRAMERATE = 10;
    Capture capture(SMALL, FRAMERATE);
    QVERIFY(capture.running);

    int sent = 0;
    int handedOver = 0;
    quint32 last = 0;
    QElapsedTimer running;
    running.start();
    while (running.elapsed() < 2000) {
        sent += capture.source->send(static_cast<quint32>(sent + 1));
        if (capture.grab(5) != Result::Frame)
            continue;
        handedOver++;
        QVERIFY(numberOf(capture.frame.get()) > last);
        last = numberOf(capture.frame.get());
    }

    qInfo() << sent << "pictures sent in two seconds," << handedOver << "handed over at" << FRAMERATE << "a second";
    QVERIFY(sent > 100);
    QVERIFY2(handedOver >= 2 * FRAMERATE - 3 && handedOver <= 2 * FRAMERATE, qPrintable(QString::number(handedOver)));
    QVERIFY(last + 30 > static_cast<quint32>(sent));
}

void TestPortalCapture::aSpoiledPictureIsPassedOver_data()
{
    QTest::addColumn<int>("spoiling");
    QTest::addColumn<bool>("onCard");
    QTest::newRow("marked corrupted in its header") << int(TestSource::Spoiling::MarkedInHeader) << false;
    QTest::newRow("marked corrupted in its chunk") << int(TestSource::Spoiling::MarkedInChunk) << false;
    QTest::newRow("of no size") << int(TestSource::Spoiling::Empty) << false;
    QTest::newRow("on the card, marked corrupted in its header") << int(TestSource::Spoiling::MarkedInHeader) << true;
    QTest::newRow("on the card, of no size") << int(TestSource::Spoiling::Empty) << true;
}

void TestPortalCapture::aSpoiledPictureIsPassedOver()
{
    QFETCH(int, spoiling);
    QFETCH(bool, onCard);
    if (onCard && !TestSource::cardMemoryCanBeMade())
        QSKIP(NO_CARD_MEMORY);
    Capture capture(SMALL, 1000, onCard);
    QVERIFY(capture.running);

    QVERIFY(capture.source->send(5, static_cast<TestSource::Spoiling>(spoiling)));
    QCOMPARE(capture.grab(200), Result::NoFrame);

    for (quint32 number = 6; number < 6 + 2 * SOURCE_BUFFERS; number++)
        QVERIFY2(capture.source->sendWhenItCan(number, static_cast<TestSource::Spoiling>(spoiling)), "a spoiled picture's buffer did not come back");
    QVERIFY(capture.source->sendWhenItCan(40));
    QCOMPARE(capture.grab(), Result::Frame);
    QCOMPARE(numberOf(capture.frame.get()), 40u);
}

void TestPortalCapture::aCutOutIsMarkedOnThePicture()
{
    Capture capture(SMALL);
    QVERIFY(capture.running);
    QVERIFY(capture.source->send(9, TestSource::Spoiling::None, QRect(4, 6, 40, 30)));
    QCOMPARE(capture.grab(), Result::Frame);

    QCOMPARE(QSize(capture.frame->width, capture.frame->height), SMALL);
    QCOMPARE(capture.frame->crop_left, size_t(4));
    QCOMPARE(capture.frame->crop_top, size_t(6));
    QCOMPARE(capture.frame->crop_right, size_t(SMALL.width() - 44));
    QCOMPARE(capture.frame->crop_bottom, size_t(SMALL.height() - 36));
    QCOMPARE(PortalCapture::sizeOf(capture.adopted), QSize(40, 30));

    QVERIFY(capture.source->send(10, TestSource::Spoiling::None, QRect(40, 30, 40, 30)));
    QCOMPARE(capture.grab(), Result::Frame);
    QCOMPARE(capture.frame->crop_left + capture.frame->crop_right + capture.frame->crop_top + capture.frame->crop_bottom, size_t(0));
}

void TestPortalCapture::aChangeOfSizeBringsPicturesOfTheNewSize()
{
    Capture capture(SMALL);
    QVERIFY(capture.running);
    QVERIFY(capture.source->send(1));
    QCOMPARE(capture.grab(), Result::Frame);
    QCOMPARE(QSize(capture.frame->width, capture.frame->height), SMALL);

    capture.source->resize(LARGER);
    QTRY_COMPARE(capture.source->agreedSize(), LARGER);
    QVERIFY(capture.source->sendWhenItCan(2));
    QCOMPARE(capture.grab(2000), Result::Frame);
    QCOMPARE(QSize(capture.frame->width, capture.frame->height), LARGER);
    QCOMPARE(pixelsThatDiffer(capture.frame.get(), 2), 0);
    QCOMPARE(PortalCapture::sizeOf(capture.adopted), LARGER);
}

void TestPortalCapture::theSizeOfASourceIsWhatItsPicturesTurnOutToBe()
{
    TestSource source(SMALL);
    const CaptureSource adopted = PortalCapture::adopt(source.node(), LARGER);
    QVERIFY(adopted.id.startsWith(QLatin1String("pipewire:")));
    QCOMPARE(adopted.geometry, QRect(QPoint(0, 0), LARGER));
    QCOMPARE(PortalCapture::sizeOf(adopted), LARGER);

    const std::unique_ptr<LinuxGrabber> grabber = PortalCapture::open(adopted.id, 30, false);
    QVERIFY(grabber);
    const FramePointer frame = emptyFrame();
    QVERIFY(firstPictureArrives(source, grabber.get(), frame.get()));
    QCOMPARE(PortalCapture::sizeOf(adopted), SMALL);

    PortalCapture::release(adopted.id);
    QCOMPARE(PortalCapture::sizeOf(adopted), QSize());
}

void TestPortalCapture::aPictureKeptStaysAsItWasWhileLaterOnesArrive()
{
    Capture capture(SMALL);
    QVERIFY(capture.running);
    QVERIFY(capture.source->send(21));
    const FramePointer kept = emptyFrame();
    QCOMPARE(capture.grabber->grab(kept.get(), milliseconds(500)), Result::Frame);

    for (quint32 number = 22; number < 22 + 3 * SOURCE_BUFFERS; number++) {
        QVERIFY(capture.source->sendWhenItCan(number));
        QCOMPARE(capture.grab(), Result::Frame);
        QCOMPARE(numberOf(capture.frame.get()), number);
    }
    QCOMPARE(pixelsThatDiffer(kept.get(), 21), 0);

    capture.grabber.reset();
    QCOMPARE(pixelsThatDiffer(kept.get(), 21), 0);
}

void TestPortalCapture::endingTheSourceIsClosed()
{
    Capture capture(SMALL);
    QVERIFY(capture.running);
    capture.source.reset();

    QTRY_COMPARE_WITH_TIMEOUT(capture.grab(100), Result::Closed, 5000);
    QCOMPARE(capture.grab(100), Result::Closed);
}

void TestPortalCapture::releasingTheSourceClosesWhatCapturesIt()
{
    Capture capture(SMALL);
    QVERIFY(capture.running);
    PortalCapture::release(capture.adopted.id);

    QCOMPARE(capture.grab(100), Result::Closed);
    QVERIFY(!PortalCapture::open(capture.adopted.id, 30, false));
}

void TestPortalCapture::aNodeThatIsNotThereFails()
{
    const CaptureSource adopted = PortalCapture::adopt(0x7ffffff0, SMALL);
    const std::unique_ptr<LinuxGrabber> grabber = PortalCapture::open(adopted.id, 30, false);
    QVERIFY(grabber);

    const FramePointer frame = emptyFrame();
    QElapsedTimer waited;
    waited.start();
    Result result = Result::NoFrame;
    while (result == Result::NoFrame && waited.elapsed() < 10000)
        result = grabber->grab(frame.get(), milliseconds(100));

    qInfo() << "Failed after" << waited.elapsed() << "ms:" << grabber->failure();
    QCOMPARE(result, Result::Failed);
    QVERIFY(!grabber->failure().isEmpty());
    QCOMPARE(grabber->name(), QStringLiteral("pipewire"));
    PortalCapture::release(adopted.id);
}

void TestPortalCapture::aSourceNobodyRegisteredCannotBeOpened()
{
    QVERIFY(!PortalCapture::open(QStringLiteral("pipewire:123456"), 30, false));
    QVERIFY(!PortalCapture::open(QStringLiteral("screen:0"), 30, false));
    CaptureSource unknown;
    unknown.id = QStringLiteral("pipewire:123456");
    QCOMPARE(PortalCapture::sizeOf(unknown), QSize());
    PortalCapture::release(unknown.id);
}

void TestPortalCapture::openingAndClosingTwentyTimesLeavesNothingBehind()
{
    TestSource source(SMALL);
    const CaptureSource adopted = PortalCapture::adopt(source.node(), SMALL);
    const FramePointer frame = emptyFrame();
    const auto captureOnce = [&] {
        const std::unique_ptr<LinuxGrabber> grabber = PortalCapture::open(adopted.id, 30, false);
        return firstPictureArrives(source, grabber.get(), frame.get());
    };

    const std::optional<int> objects = pipeWireObjectsOfThisProcess();
    if (!objects)
        QSKIP("pw-dump is not here to count what PipeWire holds for this process.");
    QTRY_COMPARE(pipeWireObjectsOfThisProcess().value_or(-1), 2);
    QVERIFY(captureOnce());
    const int threads = threadsOfThisProcess();
    const int descriptors = descriptorsOfThisProcess();
    QTRY_COMPARE(source.buffers(), 0);

    {
        const std::unique_ptr<LinuxGrabber> open = PortalCapture::open(adopted.id, 30, false);
        QTRY_VERIFY(threadsOfThisProcess() > threads);
        QTRY_VERIFY(pipeWireObjectsOfThisProcess().value_or(0) > 2);
    }

    for (int i = 0; i < 20; i++)
        QVERIFY2(captureOnce(), qPrintable(QStringLiteral("capture %1 brought no picture").arg(i)));

    QCOMPARE(threadsOfThisProcess(), threads);
    QCOMPARE(descriptorsOfThisProcess(), descriptors);
    QTRY_COMPARE(pipeWireObjectsOfThisProcess().value_or(-1), 2);
    QTRY_COMPARE(source.buffers(), 0);
    PortalCapture::release(adopted.id);
}

void TestPortalCapture::aPictureOnTheCardIsHandedOverWhereItIs()
{
    if (!TestSource::cardMemoryCanBeMade())
        QSKIP(NO_CARD_MEMORY);
    Capture capture(SMALL, 1000, true);
    QVERIFY(capture.running);
    QVERIFY(capture.source->agreedOnCard());
    QVERIFY(capture.source->send(31));
    QCOMPARE(capture.grab(), Result::Frame);

    QCOMPARE(capture.frame->format, int(AV_PIX_FMT_DRM_PRIME));
    QCOMPARE(QSize(capture.frame->width, capture.frame->height), SMALL);
    QCOMPARE(capture.grabber->name(), QStringLiteral("pipewire dmabuf BGRx"));

    const AVDRMFrameDescriptor *layout = layoutOf(capture.frame.get());
    QCOMPARE(layout->nb_objects, 1);
    QVERIFY(layout->objects[0].fd >= 0);
    QVERIFY(layout->objects[0].size >= size_t(SMALL.width() * SMALL.height() * 4));
    QCOMPARE(layout->objects[0].format_modifier, TestSource::LINEAR_MODIFIER);
    QCOMPARE(layout->nb_layers, 1);
    QCOMPARE(layout->layers[0].format, uint32_t(MKTAG('X', 'R', '2', '4')));
    QCOMPARE(layout->layers[0].nb_planes, 1);
    QCOMPARE(layout->layers[0].planes[0].object_index, 0);
    QCOMPARE(layout->layers[0].planes[0].offset, ptrdiff_t(0));
    QCOMPARE(layout->layers[0].planes[0].pitch, ptrdiff_t(SMALL.width() * 4));
    QCOMPARE(pixelsThatDifferOnCard(capture.frame.get(), 31), 0);

    QVERIFY(capture.source->send(32, TestSource::Spoiling::None, QRect(2, 2, 20, 10)));
    QCOMPARE(capture.grab(), Result::Frame);
    QCOMPARE(capture.frame->crop_left, size_t(2));
    QCOMPARE(capture.frame->crop_bottom, size_t(SMALL.height() - 12));
}

void TestPortalCapture::syncObjectsAfterAPicturesPlanesAreNotTakenForPlanes()
{
    if (!TestSource::cardMemoryCanBeMade())
        QSKIP(NO_CARD_MEMORY);
    if (!PipeWireTestSource::HAS_SYNC_OBJECTS)
        QSKIP("The PipeWire this was built with has no sync objects for a buffer to end in.");
    PipeWireTestSource source(SMALL, draw, SPA_VIDEO_FORMAT_BGRx, true, true);
    const CaptureSource adopted = PortalCapture::adopt(source.node(), SMALL);
    const std::unique_ptr<LinuxGrabber> grabber = PortalCapture::open(adopted.id, 1000, true);
    const FramePointer frame = emptyFrame();
    QVERIFY(firstPictureArrives(source, grabber.get(), frame.get(), 71));

    QCOMPARE(frame->format, int(AV_PIX_FMT_DRM_PRIME));
    QCOMPARE(layoutOf(frame.get())->nb_objects, 1);
    QCOMPARE(layoutOf(frame.get())->layers[0].nb_planes, 1);
    QCOMPARE(layoutOf(frame.get())->layers[0].planes[0].pitch, ptrdiff_t(SMALL.width() * 4));
    QCOMPARE(pixelsThatDifferOnCard(frame.get(), 71), 0);
    PortalCapture::release(adopted.id);
}

void TestPortalCapture::aPictureOnTheCardIsNotDrawnOverWhileItIsKept()
{
    if (!TestSource::cardMemoryCanBeMade())
        QSKIP(NO_CARD_MEMORY);
    Capture capture(SMALL, 1000, true);
    QVERIFY(capture.running);
    QCOMPARE(capture.source->buffers(), SOURCE_BUFFERS);

    std::vector<FramePointer> kept;
    for (quint32 number = 1; number <= SOURCE_BUFFERS - 2; number++) {
        QVERIFY(capture.source->sendWhenItCan(number));
        kept.push_back(emptyFrame());
        QCOMPARE(capture.grabber->grab(kept.back().get(), milliseconds(500)), Result::Frame);
    }
    for (quint32 number = 100; number < 100 + 3 * SOURCE_BUFFERS; number++) {
        QVERIFY(capture.source->sendWhenItCan(number));
        QCOMPARE(capture.grab(), Result::Frame);
        QCOMPARE(pixelsThatDifferOnCard(capture.frame.get(), number), 0);
    }
    for (quint32 number = 1; number <= SOURCE_BUFFERS - 2; number++)
        QCOMPARE(pixelsThatDifferOnCard(kept[number - 1].get(), number), 0);

    const AVDRMFrameDescriptor *layout = layoutOf(kept.front().get());
    const int watched = fcntl(layout->objects[0].fd, F_DUPFD_CLOEXEC, 3);
    void *mapped = mmap(nullptr, layout->objects[0].size, PROT_READ, MAP_SHARED, watched, 0);
    QVERIFY(mapped != MAP_FAILED);
    std::thread([&kept] { kept.erase(kept.begin()); }).join();
    for (quint32 number = 200; number < 200 + 3 * SOURCE_BUFFERS; number++) {
        QVERIFY(capture.source->sendWhenItCan(number));
        QCOMPARE(capture.grab(), Result::Frame);
    }
    const quint32 drawnSince = *static_cast<const quint32 *>(mapped) & 0x00ffffffu;
    munmap(mapped, layout->objects[0].size);
    ::close(watched);
    QVERIFY2(drawnSince >= 200, "the buffer of a picture no longer kept did not go back to the source");
}

void TestPortalCapture::keepingMorePicturesThanTheSourceCanSpareDoesNotStopIt()
{
    if (!TestSource::cardMemoryCanBeMade())
        QSKIP(NO_CARD_MEMORY);
    Capture capture(SMALL, 1000, true);
    QVERIFY(capture.running);

    std::vector<FramePointer> kept;
    for (quint32 number = 1; number <= 3 * SOURCE_BUFFERS; number++) {
        QVERIFY(capture.source->sendWhenItCan(number));
        kept.push_back(emptyFrame());
        QCOMPARE(capture.grabber->grab(kept.back().get(), milliseconds(500)), Result::Frame);
        QCOMPARE(kept.back()->format, int(AV_PIX_FMT_DRM_PRIME));
        QCOMPARE(pixelsThatDifferOnCard(kept.back().get(), number), 0);
    }
    for (quint32 number = 1; number <= SOURCE_BUFFERS - 2; number++)
        QCOMPARE(pixelsThatDifferOnCard(kept[number - 1].get(), number), 0);
}

void TestPortalCapture::aPictureOnTheCardOutlivesWhatCapturedIt()
{
    if (!TestSource::cardMemoryCanBeMade())
        QSKIP(NO_CARD_MEMORY);
    const FramePointer beforeResize = emptyFrame();
    const FramePointer beforeClosing = emptyFrame();
    {
        Capture capture(SMALL, 1000, true);
        QVERIFY(capture.running);
        QVERIFY(capture.source->send(51));
        QCOMPARE(capture.grabber->grab(beforeResize.get(), milliseconds(500)), Result::Frame);

        capture.source->resize(LARGER);
        QTRY_COMPARE(capture.source->agreedSize(), LARGER);
        QVERIFY(capture.source->sendWhenItCan(52));
        QCOMPARE(capture.grabber->grab(beforeClosing.get(), milliseconds(2000)), Result::Frame);
        QCOMPARE(QSize(beforeClosing->width, beforeClosing->height), LARGER);
        QCOMPARE(pixelsThatDifferOnCard(beforeResize.get(), 51), 0);

        for (quint32 number = 53; number < 53 + 2 * SOURCE_BUFFERS; number++) {
            QVERIFY(capture.source->sendWhenItCan(number));
            QCOMPARE(capture.grab(), Result::Frame);
        }
    }
    QCOMPARE(pixelsThatDifferOnCard(beforeResize.get(), 51), 0);
    QCOMPARE(pixelsThatDifferOnCard(beforeClosing.get(), 52), 0);
}

void TestPortalCapture::memoryIsUsedWhereTheCardWasNotAskedFor()
{
    if (!TestSource::cardMemoryCanBeMade())
        QSKIP(NO_CARD_MEMORY);
    TestSource source(SMALL, SPA_VIDEO_FORMAT_BGRx, true);
    const CaptureSource adopted = PortalCapture::adopt(source.node(), SMALL);
    const std::unique_ptr<LinuxGrabber> grabber = PortalCapture::open(adopted.id, 1000, false);
    const FramePointer frame = emptyFrame();
    QVERIFY(firstPictureArrives(source, grabber.get(), frame.get(), 61));

    QVERIFY(!source.agreedOnCard());
    QCOMPARE(frame->format, int(AV_PIX_FMT_BGR0));
    QCOMPARE(pixelsThatDiffer(frame.get(), 61), 0);
    QCOMPARE(grabber->name(), QStringLiteral("pipewire memfd BGRx"));
    PortalCapture::release(adopted.id);
}

void TestPortalCapture::cardBuffersAnotherConsumerSettledAreNotHandedOverAsMemory()
{
    if (!TestSource::cardMemoryCanBeMade())
        QSKIP(NO_CARD_MEMORY);
    Capture first(SMALL, 1000, true);
    QVERIFY(first.running);
    QVERIFY(first.source->agreedOnCard());

    const std::unique_ptr<LinuxGrabber> second = PortalCapture::open(first.adopted.id, 1000, false);
    const FramePointer frame = emptyFrame();
    Result result = Result::NoFrame;
    for (QElapsedTimer waited = startedTimer(); result == Result::NoFrame && waited.elapsed() < 5000;) {
        first.source->send(81);
        result = second->grab(frame.get(), milliseconds(50));
    }
    qInfo() << "Asked for memory of a stream whose buffers are on the card:" << second->failure();
    QCOMPARE(result, Result::Failed);
    QVERIFY(!second->failure().isEmpty());
    QVERIFY(first.source->agreedOnCard());
}

void TestPortalCapture::anAskingWhoseReceiverIsGoneAnswersNobodyAndLeavesNoSession()
{
    if (!PortalCapture::usable())
        QSKIP("There is no desktop portal with a screen cast on the session bus.");

    bool answered = false;
    {
        QObject receiver;
        PortalCapture::choose(CaptureSource::Kind::Screen, &receiver, [&](const CaptureSource &, const QString &) { answered = true; });
    }
    QTest::qWait(1500);
    QVERIFY(!answered);
    QCOMPARE(sessionsAtThePortal(), 0);
}

void TestPortalCapture::anAskingThePortalTurnsDownEndsWithAReasonAndNoSession()
{
    if (!PortalCapture::usable())
        QSKIP("There is no desktop portal with a screen cast on the session bus.");

    // xdg-desktop-portal refuses a token that isnt a UUID in the call before the dialog
    QString token = QStringLiteral("not-a-token");
    CaptureSource handedOver;
    QString reason;
    int answers = 0;
    {
        QObject receiver;
        PortalCapture::chooseAgain(CaptureSource::Kind::Screen, &token, &receiver, [&](const CaptureSource &source, const QString &why) {
            handedOver = source;
            reason = why;
            answers++;
        });
        for (QElapsedTimer waited = startedTimer(); answers == 0 && waited.elapsed() < 2000;)
            QTest::qWait(20);
    }
    QTest::qWait(500);
    qInfo() << "The portal's answer to a token it cannot read:" << reason;
    QCOMPARE(answers, 1);
    QVERIFY(handedOver.id.isEmpty());
    QVERIFY(!reason.isEmpty());
    QCOMPARE(token, QStringLiteral("not-a-token"));
    QCOMPARE(sessionsAtThePortal(), 0);
}

class TestPortalOfTheTests : public QObject
{
    Q_OBJECT
private slots:
    void aPortalSlowToAnswerHoldsNobodyUpAndIsAskedAgainLater();
    void aPortalThatNeverAnswersEndsTheAskingWithAReason_data();
    void aPortalThatNeverAnswersEndsTheAskingWithAReason();
    void closingThePickerBeforeThePortalHasAnsweredLeavesNoSession();
    void closingThePickerWhileTheDialogIsUpClosesTheSession();
    void theUserCallingItOffIsNoError();
    void aSourceChosenInTheDialogIsCapturedAndReleased_data();
    void aSourceChosenInTheDialogIsCapturedAndReleased();
    void aSourceIsOpenedAgainAndAgainOnThePortalsOneConnection();

private:
    struct Answer
    {
        int answers = 0;
        CaptureSource source;
        QString reason;
    };

    PortalOfTheTests portal;
};

void TestPortalOfTheTests::aPortalSlowToAnswerHoldsNobodyUpAndIsAskedAgainLater()
{
    QElapsedTimer taken = startedTimer();
    QVERIFY(!PortalCapture::usable());
    QVERIFY2(taken.elapsed() < 150, qPrintable(QString::number(taken.elapsed())));

    PortalOfTheTests::Told told;
    told.msBeforeItsProperties = 600;
    portal.tell(told);
    portal.appear();
    QVERIFY(!PortalCapture::usable());
    QCOMPARE(portal.whatWasAsked().forItsProperties, 0);

    QTest::qWait(2100);
    taken.restart();
    for (int i = 0; i < 5; i++)
        QVERIFY(!PortalCapture::usable());
    qInfo() << "Five askings of a portal that takes 600 ms to answer took" << taken.elapsed() << "ms";
    QVERIFY2(taken.elapsed() < 300, qPrintable(QString::number(taken.elapsed())));
    QTest::qWait(700);
    QCOMPARE(portal.whatWasAsked().forItsProperties, 1);

    portal.tell(PortalOfTheTests::Told());
    QTest::qWait(1500);
    QVERIFY(PortalCapture::usable());
    QCOMPARE(portal.whatWasAsked().forItsProperties, 1);

    portal.disappear();
    QVERIFY(PortalCapture::usable());
    portal.appear();
}

void TestPortalOfTheTests::aPortalThatNeverAnswersEndsTheAskingWithAReason_data()
{
    QTest::addColumn<int>("silentAt");
    QTest::addColumn<bool>("sessionToClose");
    QTest::newRow("when asked for a session") << int(PortalOfTheTests::Step::CreateSession) << false;
    QTest::newRow("when told what to offer") << int(PortalOfTheTests::Step::SelectSources) << true;
    QTest::newRow("when asked for its connection to PipeWire") << int(PortalOfTheTests::Step::OpenPipeWireRemote) << true;
}

void TestPortalOfTheTests::aPortalThatNeverAnswersEndsTheAskingWithAReason()
{
    QFETCH(int, silentAt);
    QFETCH(bool, sessionToClose);
    PortalOfTheTests::Told told;
    told.silentAt = static_cast<PortalOfTheTests::Step>(silentAt);
    told.node = 1;
    portal.tell(told);

    Answer answer;
    QObject receiver;
    const QElapsedTimer waited = startedTimer();
    PortalCapture::choose(CaptureSource::Kind::Screen, &receiver, [&answer](const CaptureSource &source, const QString &why) { answer = { answer.answers + 1, source, why }; });
    QTest::qWait(2000);
    QCOMPARE(answer.answers, 0);
    QTRY_COMPARE_WITH_TIMEOUT(answer.answers, 1, 8000);
    qInfo() << "Given up after" << waited.elapsed() << "ms:" << answer.reason;
    QVERIFY(answer.source.id.isEmpty());
    QVERIFY(!answer.reason.isEmpty());

    QTest::qWait(300);
    QCOMPARE(answer.answers, 1);
    QCOMPARE(portal.whatWasAsked().calls.contains(QStringLiteral("Session.Close")), sessionToClose);
    QCOMPARE(portal.whatWasAsked().calls.contains(QStringLiteral("Start")), told.silentAt == PortalOfTheTests::Step::OpenPipeWireRemote);
}

void TestPortalOfTheTests::closingThePickerBeforeThePortalHasAnsweredLeavesNoSession()
{
    portal.tell(PortalOfTheTests::Told());
    Answer answer;
    {
        QObject receiver;
        PortalCapture::choose(CaptureSource::Kind::Screen, &receiver, [&answer](const CaptureSource &source, const QString &why) { answer = { answer.answers + 1, source, why }; });
    }
    QTRY_VERIFY(portal.whatWasAsked().calls.contains(QStringLiteral("Session.Close")));
    QTest::qWait(300);
    QCOMPARE(portal.whatWasAsked().calls, QStringList({ QStringLiteral("CreateSession"), QStringLiteral("Session.Close") }));
    QCOMPARE(answer.answers, 0);
}

void TestPortalOfTheTests::closingThePickerWhileTheDialogIsUpClosesTheSession()
{
    PortalOfTheTests::Told told;
    told.silentAt = PortalOfTheTests::Step::Start;
    portal.tell(told);

    Answer answer;
    {
        QObject receiver;
        PortalCapture::choose(CaptureSource::Kind::Window, &receiver, [&answer](const CaptureSource &source, const QString &why) { answer = { answer.answers + 1, source, why }; });
        QTRY_VERIFY(portal.whatWasAsked().calls.contains(QStringLiteral("Start")));
        QTest::qWait(6500);
        QCOMPARE(answer.answers, 0);
        QVERIFY(!portal.whatWasAsked().calls.contains(QStringLiteral("Session.Close")));
    }
    QTRY_VERIFY(portal.whatWasAsked().calls.contains(QStringLiteral("Session.Close")));
    portal.answerStart(0);
    QTest::qWait(300);
    QCOMPARE(answer.answers, 0);
    QVERIFY(!portal.whatWasAsked().calls.contains(QStringLiteral("OpenPipeWireRemote")));
}

void TestPortalOfTheTests::theUserCallingItOffIsNoError()
{
    PortalOfTheTests::Told told;
    told.startResponse = 1;
    portal.tell(told);

    Answer answer;
    QObject receiver;
    PortalCapture::choose(CaptureSource::Kind::Screen, &receiver, [&answer](const CaptureSource &source, const QString &why) { answer = { answer.answers + 1, source, why }; });
    QTRY_COMPARE(answer.answers, 1);
    QVERIFY(answer.source.id.isEmpty());
    QCOMPARE(answer.reason, QString());
    QTRY_VERIFY(portal.whatWasAsked().calls.contains(QStringLiteral("Session.Close")));
}

void TestPortalOfTheTests::aSourceChosenInTheDialogIsCapturedAndReleased_data()
{
    QTest::addColumn<bool>("window");
    QTest::addColumn<bool>("namesItsRequestsItself");
    QTest::newRow("a screen") << false << false;
    QTest::newRow("a window") << true << false;
    QTest::newRow("a screen, from a portal that names its requests itself") << false << true;
}

void TestPortalOfTheTests::aSourceChosenInTheDialogIsCapturedAndReleased()
{
    QFETCH(bool, window);
    QFETCH(bool, namesItsRequestsItself);
    TestSource source(SMALL);
    PortalOfTheTests::Told told;
    told.node = source.node();
    told.size = SMALL;
    told.nextToken = QStringLiteral("the-token-for-next-time");
    told.namesItsRequestsItself = namesItsRequestsItself;
    portal.tell(told);

    Answer answer;
    QObject receiver;
    QString token = QStringLiteral("the-token-from-last-time");
    PortalCapture::chooseAgain(window ? CaptureSource::Kind::Window : CaptureSource::Kind::Screen, &token, &receiver, [&answer](const CaptureSource &chosen, const QString &why) { answer = { answer.answers + 1, chosen, why }; });
    QTRY_COMPARE(answer.answers, 1);
    QVERIFY2(answer.reason.isEmpty(), qPrintable(answer.reason));
    QVERIFY(answer.source.id.startsWith(QLatin1String("pipewire:")));
    QCOMPARE(answer.source.kind, window ? CaptureSource::Kind::Window : CaptureSource::Kind::Screen);
    QCOMPARE(answer.source.name, window ? QStringLiteral("Window") : QStringLiteral("Screen"));
    QCOMPARE(answer.source.geometry, QRect(QPoint(0, 0), SMALL));
    QCOMPARE(answer.source.processId, quint32(0));
    QCOMPARE(PortalCapture::sizeOf(answer.source), SMALL);
    QCOMPARE(token, QStringLiteral("the-token-for-next-time"));

    const PortalOfTheTests::Asked asked = portal.whatWasAsked();
    QCOMPARE(asked.calls, QStringList({ QStringLiteral("CreateSession"), QStringLiteral("SelectSources"), QStringLiteral("Start"), QStringLiteral("OpenPipeWireRemote") }));
    QCOMPARE(asked.sourceOptions.value(QStringLiteral("types")), QVariant(window ? 2u : 1u));
    QCOMPARE(asked.sourceOptions.value(QStringLiteral("multiple")), QVariant(false));
    QCOMPARE(asked.sourceOptions.value(QStringLiteral("cursor_mode")), QVariant(2u));
    QCOMPARE(asked.sourceOptions.value(QStringLiteral("persist_mode")), QVariant(2u));
    QCOMPARE(asked.sourceOptions.value(QStringLiteral("restore_token")), QVariant(QStringLiteral("the-token-from-last-time")));

    const std::unique_ptr<LinuxGrabber> grabber = PortalCapture::open(answer.source.id, 1000, false);
    const FramePointer frame = emptyFrame();
    QVERIFY(firstPictureArrives(source, grabber.get(), frame.get(), 91));
    QCOMPARE(pixelsThatDiffer(frame.get(), 91), 0);

    portal.closeTheSession();
    QTRY_COMPARE(grabber->grab(frame.get(), milliseconds(50)), Result::Closed);
    QVERIFY(!portal.whatWasAsked().calls.contains(QStringLiteral("Session.Close")));
    PortalCapture::release(answer.source.id);
    QTRY_VERIFY(portal.whatWasAsked().calls.contains(QStringLiteral("Session.Close")));
    QVERIFY(!PortalCapture::open(answer.source.id, 30, false));
}

void TestPortalOfTheTests::aSourceIsOpenedAgainAndAgainOnThePortalsOneConnection()
{
    TestSource source(SMALL);
    PortalOfTheTests::Told told;
    told.node = source.node();
    told.size = SMALL;
    portal.tell(told);

    Answer answer;
    QObject receiver;
    PortalCapture::choose(CaptureSource::Kind::Screen, &receiver, [&answer](const CaptureSource &chosen, const QString &why) { answer = { answer.answers + 1, chosen, why }; });
    QTRY_COMPARE(answer.answers, 1);
    QVERIFY2(!answer.source.id.isEmpty(), qPrintable(answer.reason));

    const FramePointer frame = emptyFrame();
    for (quint32 number = 1; number <= 20; number++) {
        const std::unique_ptr<LinuxGrabber> grabber = PortalCapture::open(answer.source.id, 1000, false);
        QVERIFY2(firstPictureArrives(source, grabber.get(), frame.get(), number), qPrintable(QStringLiteral("capture %1: %2").arg(number).arg(grabber->failure())));
        QCOMPARE(pixelsThatDiffer(frame.get(), number), 0);
    }
    QCOMPARE(portal.whatWasAsked().calls.count(QStringLiteral("OpenPipeWireRemote")), 1);
    PortalCapture::release(answer.source.id);
    QTRY_VERIFY(portal.whatWasAsked().calls.contains(QStringLiteral("Session.Close")));
}

void TestPortalCapture::aPortalOfTheTestsOwnIsAskedOnABusOfTheTestsOwn()
{
    // a bus that starts no service: the desktop's own would start the real portal, which shows dialogs
    QTemporaryDir place;
    QFile configuration(place.filePath(QStringLiteral("bus.conf")));
    QVERIFY(configuration.open(QIODevice::WriteOnly));
    configuration.write(QStringLiteral("<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n<busconfig><type>session</type><listen>unix:dir=%1</listen><policy context=\"default\"><allow send_destination=\"*\" eavesdrop=\"true\"/><allow eavesdrop=\"true\"/><allow own=\"*\"/></policy></busconfig>\n").arg(place.path()).toUtf8());
    configuration.close();

    QProcess bus;
    bus.start(QStringLiteral("dbus-daemon"), { QStringLiteral("--config-file=") + configuration.fileName(), QStringLiteral("--nofork"), QStringLiteral("--print-address") });
    if (!bus.waitForStarted(3000) || !bus.waitForReadyRead(3000))
        QSKIP("There is no dbus-daemon here to start a session bus of the test's own with.");
    const QByteArray address = bus.readLine().trimmed();

    QProcess onItsOwnBus;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QString::fromLatin1(address));
    onItsOwnBus.setProcessEnvironment(environment);
    onItsOwnBus.setProcessChannelMode(QProcess::MergedChannels);
    onItsOwnBus.start(QCoreApplication::applicationFilePath(), { QStringLiteral("-with-a-portal-of-its-own") });
    const bool finished = onItsOwnBus.waitForFinished(120000);
    const QByteArray said = onItsOwnBus.readAll();
    bus.terminate();
    bus.waitForFinished(3000);

    for (const QByteArray &line : said.split('\n')) {
        if (!line.startsWith("PASS") && !line.isEmpty())
            qInfo().noquote() << "    " << line;
    }
    QVERIFY2(finished, "the slots on a bus of their own did not finish in two minutes");
    QCOMPARE(onItsOwnBus.exitStatus(), QProcess::NormalExit);
    QCOMPARE(onItsOwnBus.exitCode(), 0);
    QVERIFY(said.contains("Totals:"));
}

void TestPortalCapture::theRealScreenIsCapturedInMemoryAndOnTheCard()
{
    if (!PortalCapture::usable())
        QSKIP("There is no desktop portal with a screen cast on the session bus.");
    const ApprovedScreen approved = ApprovedScreen::restore();
    if (approved.state == ApprovedScreen::State::NeverApproved)
        QSKIP(ApprovedScreen::NEVER_APPROVED);
    QVERIFY2(approved.state == ApprovedScreen::State::Restored, qPrintable(approved.problem));
    const CaptureSource screen = approved.source;

    qInfo() << "The portal handed over" << screen.id << "of" << screen.geometry.size() << "after" << approved.answeredAfterMs << "ms, which is sooner than anybody answers a dialog";
    for (const bool onCard : { false, true }) {
        const std::unique_ptr<LinuxGrabber> grabber = PortalCapture::open(screen.id, 30, onCard);
        QVERIFY(grabber);
        const FramePointer frame = emptyFrame();
        Result first = Result::NoFrame;
        QElapsedTimer waited;
        waited.start();
        while (first == Result::NoFrame && waited.elapsed() < 6000)
            first = grabber->grab(frame.get(), milliseconds(100));
        QVERIFY2(first == Result::Frame, qPrintable(grabber->failure()));
        QCOMPARE(QSize(frame->width, frame->height), screen.geometry.size());
        QCOMPARE(PortalCapture::sizeOf(screen), screen.geometry.size());

        if (frame->format == AV_PIX_FMT_DRM_PRIME) {
            const AVDRMFrameDescriptor *layout = layoutOf(frame.get());
            const uint32_t fourcc = layout->layers[0].format;
            qInfo().noquote() << QStringLiteral("On the card: fourcc %1, modifier 0x%2, %3 object(s) of %4 bytes, %5 plane(s), offset %6, pitch %7").arg(QString::fromLatin1(reinterpret_cast<const char *>(&fourcc), 4)).arg(layout->objects[0].format_modifier, 0, 16).arg(layout->nb_objects).arg(layout->objects[0].size).arg(layout->layers[0].nb_planes).arg(layout->layers[0].planes[0].offset).arg(layout->layers[0].planes[0].pitch);
        }

        int pictures = 1;
        waited.restart();
        while (waited.elapsed() < 1000)
            pictures += grabber->grab(frame.get(), milliseconds(50)) == Result::Frame;
        qInfo().noquote() << (onCard ? "Asked for the card:" : "Asked for memory:") << grabber->name() << "handed over" << pictures << "pictures in a second";
        QVERIFY(pictures <= 31);
        if (onCard && frame->format != AV_PIX_FMT_DRM_PRIME)
            qWarning("The desktop did not leave its pictures on the card although it was asked to.");
        if (frame->format != AV_PIX_FMT_DRM_PRIME)
            continue;

        const FramePointer kept = emptyFrame();
        av_frame_move_ref(kept.get(), frame.get());
        const std::optional<size_t> asKept = hashOnCard(kept.get());
        int whileKept = 0;
        waited.restart();
        while (waited.elapsed() < 1500)
            whileKept += grabber->grab(frame.get(), milliseconds(50)) == Result::Frame;
        const std::optional<size_t> afterwards = hashOnCard(kept.get());
        qInfo() << "While one picture was kept" << whileKept << "more came in a second and a half; the kept one" << (!asKept ? "cannot be read back" : asKept == afterwards ? "stayed as it was"
                                                                                                                                                                            : "was drawn over");
        QVERIFY2(whileKept > 0 || pictures < 5, "the desktop stopped sending while a picture of its was kept");
        if (asKept)
            QCOMPARE(afterwards, asKept);
    }
    PortalCapture::release(screen.id);
    QTest::qWait(200);
}

namespace {

int chooseScreenOnce()
{
    QString token;
    CaptureSource screen;
    QString reason;
    QObject receiver;
    PortalCapture::chooseAgain(CaptureSource::Kind::Screen, &token, &receiver, [&](const CaptureSource &source, const QString &why) {
        screen = source;
        reason = why;
        QCoreApplication::quit();
    });
    QCoreApplication::exec();

    PortalCapture::release(screen.id);
    QCoreApplication::processEvents();
    if (token.isEmpty()) {
        fprintf(stderr, "Nothing was approved to be restored (%s). Choose a screen and tick that the choice may be restored.\n", qPrintable(screen.id.isEmpty() ? reason : QStringLiteral("the desktop gave no token")));
        return 1;
    }
    ApprovedScreen::remember(token);
    printf("The approval of %s is stored; the tests can capture it without asking.\n", qPrintable(screen.id));
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("-choose-screen")))
        return chooseScreenOnce();
    if (application.arguments().contains(QStringLiteral("-with-a-portal-of-its-own"))) {
        TestPortalOfTheTests onABusOfItsOwn;
        return QTest::qExec(&onABusOfItsOwn, QStringList({ application.arguments().first(), QStringLiteral("-nocrashhandler") }));
    }
    TestPortalCapture test;
    return QTest::qExec(&test, argc, argv);
}

#else

class TestPortalCapture : public QObject
{
    Q_OBJECT
private slots:
    void thisBuildHasNoPortalCapture() { QSKIP("This build has no PipeWire or no D-Bus, so it cannot capture through the desktop portal."); }
};

QTEST_GUILESS_MAIN(TestPortalCapture)

#endif

#include "tst_PortalCapture.moc"
