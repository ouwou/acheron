#include <QtTest>

#include "Core/Video/LinuxCapture.hpp"

#if defined(ACHERON_HAVE_XCB) && defined(ACHERON_HAVE_AVDEVICE)
#include "ProcessUsage.hpp"
#include "X11TestWindow.hpp"

#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include <unistd.h>

#include <xcb/randr.h>
#include <xcb/xcb.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#endif

using namespace Acheron;
using namespace Acheron::Core::Video;
using ProcessUsage::processCpuMs;

#if defined(ACHERON_HAVE_XCB) && defined(ACHERON_HAVE_AVDEVICE)

namespace {

using Result = LinuxGrabber::Result;
using std::chrono::milliseconds;

const char *const NO_DISPLAY = "no X server that shows the desktop can be reached: there is none, or it is Xwayland";
const char *const NO_WINDOW_MANAGER = "no window manager runs on this display, and it is one that minimises windows and lists them";

constexpr quint32 RED = 0xC8281E;
constexpr quint32 GREEN = 0x1EB43C;
constexpr quint32 BLUE = 0x2846D2;
constexpr quint32 ORANGE = 0xF08C14;
constexpr double NEARLY_ALL = 0.95;
constexpr int SETTLE_MS = 300;

using TestWindow = X11TestWindow;
template <typename T>
using Reply = X11TestWindow::Reply<T>;

struct FrameDeleter
{
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
using FramePointer = std::unique_ptr<AVFrame, FrameDeleter>;

double shareOf(const AVFrame *frame, quint32 colour, QRect area = QRect())
{
    const QRect whole(0, 0, frame->width, frame->height);
    if (area.isNull())
        area = whole;
    const QRect inside = area.intersected(whole);
    if (area.isEmpty() || frame->format != AV_PIX_FMT_BGR0)
        return 0;

    qint64 same = 0;
    for (int y = inside.top(); y <= inside.bottom(); y++) {
        const auto *row = reinterpret_cast<const quint32 *>(frame->data[0] + static_cast<qsizetype>(y) * frame->linesize[0]);
        for (int x = inside.left(); x <= inside.right(); x++)
            same += (row[x] & 0x00FFFFFFu) == colour;
    }
    return static_cast<double>(same) / (static_cast<double>(area.width()) * area.height());
}

QSize sizeOf(const AVFrame *frame)
{
    return QSize(frame->width, frame->height);
}

Result grabUntil(LinuxGrabber &grabber, AVFrame *frame, Result wanted, int ms = 3000)
{
    QElapsedTimer timer;
    timer.start();
    Result last;
    do
        last = grabber.grab(frame, milliseconds(100));
    while (last != wanted && timer.elapsed() < ms);
    return last;
}

bool showsWithin(LinuxGrabber &grabber, AVFrame *frame, quint32 colour, const QRect &area = QRect())
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        if (grabber.grab(frame, milliseconds(100)) == Result::Frame && shareOf(frame, colour, area) >= NEARLY_ALL)
            return true;
    }
    return false;
}

struct Pace
{
    int pictures = 0;
    double seconds = 0;
    double cpuMs = 0;
    QSize size;
};

Pace paceOf(LinuxGrabber &grabber, int ms)
{
    const FramePointer frame(av_frame_alloc());
    Pace pace;
    if (grabUntil(grabber, frame.get(), Result::Frame) != Result::Frame)
        return pace;

    const double cpuBefore = processCpuMs();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        if (grabber.grab(frame.get(), milliseconds(100)) == Result::Frame)
            pace.pictures++;
    }
    pace.seconds = static_cast<double>(timer.nsecsElapsed()) / 1e9;
    pace.cpuMs = processCpuMs() - cpuBefore;
    pace.size = sizeOf(frame.get());
    return pace;
}

} // namespace

class TestX11Capture : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();

    void usableIsAnsweredTheSameEveryTimeAndAtOnce();
    void screensAreListedTheSameEveryTime();
    void aScreenIsCapturedWithAWindowWhereItIs();
    void aPictureStaysAsItWasWhileItIsKept();
    void theCursorIsDrawnIntoThePicture();

    void windowsAreListedWithoutThisProgramsOwn();
    void otherProgramsWindowsAreListedTopFirst();
    void onlyWindowsAUserWouldShareAreListed();

    void aWindowIsCapturedWhateverCoversIt();
    void aWindowWithOpacityOfItsOwnIsCapturedInItsColours();
    void aWindowIsCapturedWithoutTheShadowItDrawsItself();
    void aShadowThatDoesNotFitItsWindowIsIgnored_data();
    void aShadowThatDoesNotFitItsWindowIsIgnored();
    void aWindowThatCannotBeReadFails();
    void aWindowMadeSmallerAsItIsReadIsReadAgain();
    void aWindowIsFollowedWhenItMoves();
    void aResizedWindowIsCapturedAtItsNewSizeOnceThatHasHeld();
    void aHiddenWindowIsHiddenUntilItIsShownAgain();
    void aMinimisedWindowIsHiddenUntilItIsRestored();
    void aClosedWindowEndsItsCapture();
    void aSourceThatIsNotThereFails_data();
    void aSourceThatIsNotThereFails();
    void aThumbnailHasTheWindowsColour();

    void aGrabWaitsNoLongerThanItWasAskedTo();
    void theFrameRateAskedForIsKept_data();
    void theFrameRateAskedForIsKept();

private:
    [[nodiscard]] TestWindow::Look look(const QSize &size, quint32 colour, const QPoint &offset = QPoint(80, 80)) const;
    [[nodiscard]] bool aWindowManagerRuns() const { return TestWindow::aWindowManagerRuns(*display); }
    [[nodiscard]] QString titleFor(const char *what) const { return QStringLiteral("Acheron capture test %1 %2").arg(getpid()).arg(QLatin1String(what)); }
    static bool isListed(const QString &title, CaptureSource *found = nullptr);

    std::unique_ptr<TestWindow::Display> display;
    xcb_connection_t *connection = nullptr;
    xcb_window_t root = XCB_NONE;
    CaptureSource quietScreen;
};

void TestX11Capture::initTestCase()
{
    if (!X11Capture::usable())
        return;

    display = std::make_unique<TestWindow::Display>();
    QVERIFY(display->isOpen());
    connection = display->connection;
    root = display->root;

    const QList<CaptureSource> screens = X11Capture::screens();
    QVERIFY(!screens.isEmpty());
    quietScreen = screens.last();
    qInfo() << "windows go on" << quietScreen.id << quietScreen.geometry << (aWindowManagerRuns() ? "under a window manager" : "without a window manager");
}

TestWindow::Look TestX11Capture::look(const QSize &size, quint32 colour, const QPoint &offset) const
{
    TestWindow::Look look;
    look.inside = size;
    look.at = quietScreen.geometry.topLeft() + offset;
    look.colour = colour;
    return look;
}

bool TestX11Capture::isListed(const QString &title, CaptureSource *found)
{
    for (const CaptureSource &window : X11Capture::windows()) {
        if (window.name != title)
            continue;
        if (found)
            *found = window;
        return true;
    }
    return false;
}

void TestX11Capture::usableIsAnsweredTheSameEveryTimeAndAtOnce()
{
    const bool usable = X11Capture::usable();
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 1000; i++)
        QCOMPARE(X11Capture::usable(), usable);
    QVERIFY2(timer.elapsed() < 20, qPrintable(QStringLiteral("a thousand answers took %1 ms").arg(timer.elapsed())));
    qInfo() << "usable:" << usable << "on DISPLAY" << qgetenv("DISPLAY");

    if (!usable) {
        QVERIFY(X11Capture::screens().isEmpty());
        QVERIFY(X11Capture::windows().isEmpty());
    }
}

void TestX11Capture::screensAreListedTheSameEveryTime()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    const QRect whole = TestWindow::onRoot(*display, root);
    const QList<CaptureSource> screens = X11Capture::screens();
    QVERIFY(!screens.isEmpty());
    for (int i = 0; i < screens.size(); i++) {
        qInfo() << screens[i].id << screens[i].name << screens[i].geometry;
        QVERIFY(screens[i].id.size() > QStringLiteral("screen:").size() && screens[i].id.startsWith(QStringLiteral("screen:")));
        QCOMPARE(screens[i].name, QStringLiteral("Screen %1").arg(i + 1));
        QCOMPARE(screens[i].kind, CaptureSource::Kind::Screen);
        QCOMPARE(screens[i].processId, 0u);
        QVERIFY(!screens[i].geometry.isEmpty());
        QVERIFY(whole.contains(screens[i].geometry));
        QCOMPARE(X11Capture::sizeOf(screens[i]), screens[i].geometry.size());
    }

    if (xcb_get_extension_data(connection, &xcb_randr_id)->present) {
        const Reply<xcb_randr_get_monitors_reply_t> monitors(xcb_randr_get_monitors_reply(connection, xcb_randr_get_monitors(connection, root, 1), nullptr));
        QVERIFY(monitors);
        QCOMPARE(screens.size(), xcb_randr_get_monitors_monitors_length(monitors.get()));
        for (xcb_randr_monitor_info_iterator_t each = xcb_randr_get_monitors_monitors_iterator(monitors.get()); each.rem > 0; xcb_randr_monitor_info_next(&each)) {
            if (each.data->primary)
                QCOMPARE(screens.first().geometry, QRect(each.data->x, each.data->y, each.data->width, each.data->height));
        }
    }
    for (int i = 2; i < screens.size(); i++)
        QVERIFY(screens[i - 1].geometry.x() < screens[i].geometry.x() || (screens[i - 1].geometry.x() == screens[i].geometry.x() && screens[i - 1].geometry.y() <= screens[i].geometry.y()));

    for (int again = 0; again < 5; again++) {
        const QList<CaptureSource> listed = X11Capture::screens();
        QCOMPARE(listed.size(), screens.size());
        for (int i = 0; i < listed.size(); i++) {
            QCOMPARE(listed[i].id, screens[i].id);
            QCOMPARE(listed[i].geometry, screens[i].geometry);
        }
    }
}

void TestX11Capture::aScreenIsCapturedWithAWindowWhereItIs()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    const TestWindow window(look(QSize(400, 300), RED));
    QTRY_VERIFY(window.isShown());

    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(quietScreen.id, 30);
    QVERIFY(grabber);
    QCOMPARE(grabber->name(), QStringLiteral("x11grab"));
    QVERIFY(!grabber->deliversOnlyChanges());

    const FramePointer frame(av_frame_alloc());
    const QRect place = window.insideOnScreen().translated(-quietScreen.geometry.topLeft());
    QVERIFY2(showsWithin(*grabber, frame.get(), RED, place), qPrintable(grabber->failure()));
    QCOMPARE(sizeOf(frame.get()), quietScreen.geometry.size());
    QCOMPARE(frame->format, static_cast<int>(AV_PIX_FMT_BGR0));
    QCOMPARE(frame->linesize[0], frame->width * 4);
    QVERIFY(frame->buf[0]);
    QVERIFY(shareOf(frame.get(), RED) < 0.5);
    QVERIFY(grabber->failure().isEmpty());
}

void TestX11Capture::aPictureStaysAsItWasWhileItIsKept()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    TestWindow window(look(QSize(400, 300), RED));
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, 30);
    const FramePointer kept(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, kept.get(), RED), qPrintable(grabber->failure()));

    window.paint(GREEN);
    const FramePointer newer(av_frame_alloc());
    for (int i = 0; i < 10; i++)
        QVERIFY(showsWithin(*grabber, newer.get(), GREEN));
    QVERIFY(newer->data[0] != kept->data[0]);
    QVERIFY(shareOf(kept.get(), RED) >= NEARLY_ALL);
}

void TestX11Capture::theCursorIsDrawnIntoThePicture()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    const TestWindow window(look(QSize(400, 300), BLUE));
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, 30);
    const FramePointer frame(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, frame.get(), BLUE), qPrintable(grabber->failure()));

    const Reply<xcb_query_pointer_reply_t> before(xcb_query_pointer_reply(connection, xcb_query_pointer(connection, root), nullptr));
    QVERIFY(before);
    xcb_warp_pointer(connection, XCB_NONE, window.handle(), 0, 0, 0, 0, 200, 150);
    xcb_flush(connection);

    bool drawn = false;
    QElapsedTimer timer;
    timer.start();
    while (!drawn && timer.elapsed() < 3000)
        drawn = grabber->grab(frame.get(), milliseconds(100)) == Result::Frame && shareOf(frame.get(), BLUE) < 1.0;
    const double left = shareOf(frame.get(), BLUE);

    xcb_warp_pointer(connection, XCB_NONE, root, 0, 0, 0, 0, before->root_x, before->root_y);
    xcb_flush(connection);
    QVERIFY(drawn);
    QVERIFY(left >= NEARLY_ALL);
}

void TestX11Capture::windowsAreListedWithoutThisProgramsOwn()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    const TestWindow own(look(QSize(320, 240), RED));
    QTRY_VERIFY(own.isShown());
    QTest::qWait(200);

    // window titles stay out of the log
    const QList<CaptureSource> windows = X11Capture::windows();
    qInfo() << windows.size() << "windows listed";
    QStringList ids;
    for (const CaptureSource &window : windows) {
        QCOMPARE(window.kind, CaptureSource::Kind::Window);
        QVERIFY(window.id.startsWith(QStringLiteral("window:")));
        QVERIFY(!ids.contains(window.id));
        ids.append(window.id);
        QVERIFY(!window.name.isEmpty());
        QVERIFY(window.processId != static_cast<quint32>(getpid()));
    }
    QVERIFY(!ids.contains(own.source().id));
}

void TestX11Capture::otherProgramsWindowsAreListedTopFirst()
{
    if (!connection)
        QSKIP(NO_DISPLAY);
    if (!aWindowManagerRuns())
        QSKIP(NO_WINDOW_MANAGER);
    const QString xmessage = QStandardPaths::findExecutable(QStringLiteral("xmessage"));
    const QString xterm = QStandardPaths::findExecutable(QStringLiteral("xterm"));
    if (xmessage.isEmpty() || xterm.isEmpty())
        QSKIP("xmessage and xterm, which are the other programs here, are not both installed");

    const QPoint corner = quietScreen.geometry.topLeft() + QPoint(120, 120);
    const QString below = titleFor("below");
    const QString above = titleFor("above");
    QProcess first;
    first.start(xmessage, { QStringLiteral("-title"), below, QStringLiteral("-geometry"), QStringLiteral("300x200+%1+%2").arg(corner.x()).arg(corner.y()), QStringLiteral("a window") });
    CaptureSource messages;
    QTRY_VERIFY(isListed(below, &messages));
    QProcess second;
    second.start(xterm, { QStringLiteral("-T"), above, QStringLiteral("-geometry"), QStringLiteral("40x10+%1+%2").arg(corner.x() + 200).arg(corner.y() + 100), QStringLiteral("-e"), QStringLiteral("sleep"), QStringLiteral("60") });
    CaptureSource terminal;
    QTRY_VERIFY(isListed(above, &terminal));
    QVERIFY(isListed(below, &messages));

    QCOMPARE(messages.kind, CaptureSource::Kind::Window);
    QCOMPARE(messages.geometry.size(), QSize(300, 200));
    QVERIFY(quietScreen.geometry.contains(messages.geometry));
    QCOMPARE(messages.geometry, TestWindow::onRoot(*display, messages.id.mid(7).toUInt()));
    // xmessage doesnt say which process it is
    QCOMPARE(messages.processId, 0u);
    QCOMPARE(X11Capture::sizeOf(messages), QSize(300, 200));

    QCOMPARE(terminal.processId, static_cast<quint32>(second.processId()));
    QVERIFY(!terminal.geometry.isEmpty());
    QCOMPARE(terminal.geometry, TestWindow::onRoot(*display, terminal.id.mid(7).toUInt()));

    const std::vector<quint32> stacked = TestWindow::numbers(connection, root, "_NET_CLIENT_LIST_STACKING");
    if (!stacked.empty()) {
        QStringList topDown;
        for (auto window = stacked.rbegin(); window != stacked.rend(); ++window)
            topDown.append(QStringLiteral("window:%1").arg(*window));
        QStringList listed;
        for (const CaptureSource &window : X11Capture::windows())
            listed.append(window.id);
        QVERIFY(listed.indexOf(terminal.id) >= 0 && listed.indexOf(messages.id) >= 0);
        QCOMPARE(listed.indexOf(terminal.id) < listed.indexOf(messages.id), topDown.indexOf(terminal.id) < topDown.indexOf(messages.id));
        qInfo() << "the window opened last is" << (listed.indexOf(terminal.id) < listed.indexOf(messages.id) ? "listed first" : "listed second");
    }

    TestWindow::minimise(*display, messages.id.mid(7).toUInt());
    QTRY_COMPARE(X11Capture::sizeOf(messages), QSize());
    QVERIFY(isListed(below));
    QVERIFY(X11Capture::thumbnail(messages, QSize(160, 90)).isNull());
    X11Capture::restoreIfMinimised(messages);
    QTRY_COMPARE(X11Capture::sizeOf(messages), QSize(300, 200));
    QVERIFY(!X11Capture::thumbnail(messages, QSize(160, 90)).isNull());
}

void TestX11Capture::onlyWindowsAUserWouldShareAreListed()
{
    if (!connection)
        QSKIP(NO_DISPLAY);
    if (aWindowManagerRuns())
        QSKIP("a window manager marks the windows here, and would mark these its own way");

    struct Case
    {
        const char *what;
        bool listed;
        quint32 state = TestWindow::NORMAL_STATE;
        const char *type = nullptr;
        bool skipsTaskbar = false;
        bool ours = false;
        bool titled = true;
        bool framed = false;
        bool marked = true;
    };
    const Case cases[] = {
        { "a program's window", true },
        { "a minimised window", true, TestWindow::ICONIC_STATE },
        { "a dialog", true, TestWindow::NORMAL_STATE, "_NET_WM_WINDOW_TYPE_DIALOG" },
        { "a window inside a frame", true, TestWindow::NORMAL_STATE, nullptr, false, false, true, true },
        { "a withdrawn window", false, TestWindow::WITHDRAWN_STATE },
        { "a dock", false, TestWindow::NORMAL_STATE, "_NET_WM_WINDOW_TYPE_DOCK" },
        { "a desktop", false, TestWindow::NORMAL_STATE, "_NET_WM_WINDOW_TYPE_DESKTOP" },
        { "a notification", false, TestWindow::NORMAL_STATE, "_NET_WM_WINDOW_TYPE_NOTIFICATION" },
        { "a window kept off the taskbar", false, TestWindow::NORMAL_STATE, nullptr, true },
        { "this program's own window", false, TestWindow::NORMAL_STATE, nullptr, false, true },
        { "a window without a title", false, TestWindow::NORMAL_STATE, nullptr, false, false, false },
        { "a window no window manager looks after", false, TestWindow::NORMAL_STATE, nullptr, false, false, true, false, false },
    };

    int number = 0;
    for (const Case &each : cases) {
        TestWindow::Look frame = look(QSize(260, 200), GREEN, QPoint(60 + 20 * number, 60 + 20 * number));
        frame.ours = false;
        frame.title.clear();
        const TestWindow around(frame);

        TestWindow::Look inside = look(QSize(240, 160), RED, QPoint(60 + 20 * number, 60 + 20 * number));
        inside.title = each.titled ? titleFor(each.what) : QString();
        inside.ours = each.ours;
        if (each.framed) {
            inside.within = around.handle();
            inside.at = QPoint(10, 30);
        }
        TestWindow window(inside);
        if (each.marked)
            window.setState(each.state);
        if (each.type)
            window.setAtoms("_NET_WM_WINDOW_TYPE", each.type);
        if (each.skipsTaskbar)
            window.setAtoms("_NET_WM_STATE", "_NET_WM_STATE_SKIP_TASKBAR");
        QTRY_VERIFY(window.isShown());

        bool listed = false;
        for (const CaptureSource &source : X11Capture::windows()) {
            if (source.id != window.source().id)
                continue;
            listed = true;
            QCOMPARE(source.name, inside.title);
            QCOMPARE(source.kind, CaptureSource::Kind::Window);
            QCOMPARE(source.geometry, window.insideOnScreen());
            QCOMPARE(source.processId, 0u);
        }
        QVERIFY2(listed == each.listed, each.what);
        number++;
    }
}

void TestX11Capture::aWindowIsCapturedWhateverCoversIt()
{
    if (!connection)
        QSKIP(NO_DISPLAY);
    const Reply<xcb_query_extension_reply_t> composite(xcb_query_extension_reply(connection, xcb_query_extension(connection, 9, "Composite"), nullptr));
    if (!composite || !composite->present)
        QSKIP("this X server keeps no picture of a window that something covers: it has no Composite extension");

    const TestWindow window(look(QSize(400, 300), BLUE));
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, 30);
    const FramePointer frame(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, frame.get(), BLUE), qPrintable(grabber->failure()));
    QCOMPARE(sizeOf(frame.get()), window.insideOnScreen().size());

    const TestWindow cover(look(QSize(600, 500), GREEN, QPoint(40, 40)));
    const std::unique_ptr<LinuxGrabber> wholeScreen = X11Capture::open(quietScreen.id, 30);
    const FramePointer seen(av_frame_alloc());
    const QRect place = window.insideOnScreen().translated(-quietScreen.geometry.topLeft());
    QVERIFY2(showsWithin(*wholeScreen, seen.get(), GREEN, place), "the second window did not come to lie over the first");

    for (int i = 0; i < 10; i++) {
        QCOMPARE(grabUntil(*grabber, frame.get(), Result::Frame), Result::Frame);
        QVERIFY(shareOf(frame.get(), BLUE) >= NEARLY_ALL);
    }
}

void TestX11Capture::aWindowWithOpacityOfItsOwnIsCapturedInItsColours()
{
    if (!connection)
        QSKIP(NO_DISPLAY);
    TestWindow::Look opaque = look(QSize(400, 300), GREEN);
    opaque.ownOpacity = true;
    if (!TestWindow::visualWithOpacity(*display))
        QSKIP("this X server has no windows with opacity of their own: none of its visuals is 32 bits deep");

    const TestWindow window(opaque);
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, 30);
    const FramePointer frame(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, frame.get(), GREEN), qPrintable(grabber->failure()));
    QCOMPARE(frame->format, static_cast<int>(AV_PIX_FMT_BGR0));
    QCOMPARE(sizeOf(frame.get()), QSize(400, 300));
    const quint32 ofItsOwn = *reinterpret_cast<const quint32 *>(frame->data[0]) >> 24;

    const TestWindow plain(look(QSize(400, 300), GREEN, QPoint(500, 80)));
    const std::unique_ptr<LinuxGrabber> ofThePlain = X11Capture::open(plain.source().id, 30);
    QVERIFY2(showsWithin(*ofThePlain, frame.get(), GREEN), qPrintable(ofThePlain->failure()));
    qInfo() << "the fourth byte of a pixel is" << ofItsOwn << "in a window with opacity of its own and" << (*reinterpret_cast<const quint32 *>(frame->data[0]) >> 24) << "in one without";
}

void TestX11Capture::aWindowIsCapturedWithoutTheShadowItDrawsItself()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    // as GTK has it: this much of the window's left, right, top and bottom is shadow
    TestWindow::Look outside = look(QSize(480, 360), GREEN);
    outside.title = titleFor("with a shadow");
    outside.ours = false;
    outside.shown = false;
    TestWindow window(outside);
    window.set("_GTK_FRAME_EXTENTS", XCB_ATOM_CARDINAL, { 30, 50, 20, 40 });
    TestWindow::Look within;
    within.inside = QSize(400, 300);
    within.at = QPoint(30, 20);
    within.colour = RED;
    within.within = window.handle();
    TestWindow content(within);
    window.show();
    QTRY_VERIFY(window.isShown());
    if (!aWindowManagerRuns())
        window.setState(TestWindow::NORMAL_STATE);

    CaptureSource listed;
    QTRY_VERIFY(isListed(outside.title, &listed));
    QCOMPARE(listed.id, window.source().id);
    QCOMPARE(listed.geometry, content.insideOnScreen());
    QCOMPARE(X11Capture::sizeOf(listed), QSize(400, 300));

    const QImage shrunk = X11Capture::thumbnail(listed, QSize(200, 200));
    QCOMPARE(shrunk.size(), QSize(200, 150));
    QCOMPARE(shrunk.pixel(0, 0), 0xFF000000u | RED);
    QCOMPARE(shrunk.pixel(199, 149), 0xFF000000u | RED);

    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(listed.id, 30);
    const FramePointer frame(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, frame.get(), RED), qPrintable(grabber->failure()));
    QCOMPARE(sizeOf(frame.get()), QSize(400, 300));

    content.moveTo(QPoint(60, 40));
    window.set("_GTK_FRAME_EXTENTS", XCB_ATOM_CARDINAL, { 60, 20, 40, 20 });
    QTest::qWait(SETTLE_MS + 200);
    QVERIFY(showsWithin(*grabber, frame.get(), RED));
    QCOMPARE(sizeOf(frame.get()), QSize(400, 300));
    QCOMPARE(X11Capture::sizeOf(listed), QSize(400, 300));

    content.moveTo(QPoint(0, 0));
    content.resizeInside(QSize(480, 360));
    window.set("_GTK_FRAME_EXTENTS", XCB_ATOM_CARDINAL, { 0, 0, 0, 0 });
    QElapsedTimer sinceChange;
    sinceChange.start();
    while (sinceChange.elapsed() < 3000 && sizeOf(frame.get()) != QSize(480, 360)) {
        if (grabber->grab(frame.get(), milliseconds(20)) == Result::Frame)
            QVERIFY(sizeOf(frame.get()) == QSize(400, 300) || sizeOf(frame.get()) == QSize(480, 360));
    }
    QCOMPARE(sizeOf(frame.get()), QSize(480, 360));
    QVERIFY(sinceChange.elapsed() >= SETTLE_MS);
    QVERIFY(sinceChange.elapsed() < SETTLE_MS + 700);
    QVERIFY(showsWithin(*grabber, frame.get(), RED));
    QCOMPARE(X11Capture::sizeOf(listed), QSize(480, 360));
}

void TestX11Capture::aShadowThatDoesNotFitItsWindowIsIgnored_data()
{
    QTest::addColumn<QList<quint32>>("sides");
    QTest::newRow("as wide as the window") << QList<quint32>{ 240, 240, 0, 0 };
    QTest::newRow("wider than the window") << QList<quint32>{ 300, 300, 0, 0 };
    QTest::newRow("taller than the window") << QList<quint32>{ 0, 0, 200, 200 };
    QTest::newRow("too large to count with") << QList<quint32>{ 0x80000000u, 0, 0, 0 };
    QTest::newRow("as large as numbers get") << QList<quint32>{ 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    QTest::newRow("adding up to nothing") << QList<quint32>{ 0xFFFFFFFFu, 1, 0x80000000u, 0x80000000u };
    QTest::newRow("too few of them") << QList<quint32>{ 30, 50 };
}

void TestX11Capture::aShadowThatDoesNotFitItsWindowIsIgnored()
{
    if (!connection)
        QSKIP(NO_DISPLAY);
    QFETCH(QList<quint32>, sides);

    TestWindow::Look whole = look(QSize(480, 360), RED);
    whole.title = titleFor("with a shadow that does not fit");
    whole.ours = false;
    whole.shown = false;
    TestWindow window(whole);
    window.set("_GTK_FRAME_EXTENTS", XCB_ATOM_CARDINAL, std::vector<quint32>(sides.begin(), sides.end()));
    window.show();
    QTRY_VERIFY(window.isShown());
    if (!aWindowManagerRuns())
        window.setState(TestWindow::NORMAL_STATE);

    CaptureSource listed;
    QTRY_VERIFY(isListed(whole.title, &listed));
    QCOMPARE(listed.geometry, window.insideOnScreen());
    QCOMPARE(X11Capture::sizeOf(listed), QSize(480, 360));
    QCOMPARE(X11Capture::thumbnail(listed, QSize(1000, 1000)).size(), QSize(480, 360));

    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(listed.id, 30);
    const FramePointer frame(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, frame.get(), RED), qPrintable(grabber->failure()));
    QCOMPARE(sizeOf(frame.get()), QSize(480, 360));
}

void TestX11Capture::aWindowThatCannotBeReadFails()
{
    if (!connection)
        QSKIP(NO_DISPLAY);
    const Reply<xcb_query_extension_reply_t> composite(xcb_query_extension_reply(connection, xcb_query_extension(connection, 9, "Composite"), nullptr));
    if (composite && composite->present)
        QSKIP("this X server keeps a picture of every window being captured, so there is none it cannot read: it has the Composite extension");

    TestWindow::Look look;
    look.inside = QSize(400, 300);
    look.at = QPoint(-100, 80);
    look.colour = RED;
    look.framed = false;
    const TestWindow window(look);
    QTRY_VERIFY(window.isShown());

    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, 30);
    const FramePointer frame(av_frame_alloc());
    QElapsedTimer timer;
    timer.start();
    Result last = Result::NoFrame;
    while (last == Result::NoFrame && timer.elapsed() < 3000)
        last = grabber->grab(frame.get(), milliseconds(100));
    qInfo() << "failed after" << timer.elapsed() << "ms with:" << grabber->failure();
    QCOMPARE(last, Result::Failed);
    QVERIFY(!grabber->failure().isEmpty());
    QCOMPARE(grabber->grab(frame.get(), milliseconds(0)), Result::Failed);
}

void TestX11Capture::aWindowMadeSmallerAsItIsReadIsReadAgain()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    TestWindow window(look(QSize(400, 300), RED));
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, 5);
    const FramePointer frame(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, frame.get(), RED), qPrintable(grabber->failure()));

    for (const QSize &smaller : { QSize(360, 260), QSize(320, 220) }) {
        QCOMPARE(grabUntil(*grabber, frame.get(), Result::Frame), Result::Frame);
        std::thread shrinking([&window, smaller] {
            std::this_thread::sleep_for(milliseconds(60));
            window.resizeInside(smaller);
        });
        const Result asItShrank = grabber->grab(frame.get(), milliseconds(1000));
        shrinking.join();
        QCOMPARE(asItShrank, Result::NoFrame);

        Result last = Result::NoFrame;
        QElapsedTimer timer;
        timer.start();
        while (last != Result::Failed && sizeOf(frame.get()) != smaller && timer.elapsed() < 3000)
            last = grabber->grab(frame.get(), milliseconds(100));
        QVERIFY2(last != Result::Failed, qPrintable(grabber->failure()));
        QCOMPARE(sizeOf(frame.get()), smaller);
    }
}

void TestX11Capture::aWindowIsFollowedWhenItMoves()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    TestWindow window(look(QSize(400, 300), ORANGE));
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, 30);
    const FramePointer frame(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, frame.get(), ORANGE), qPrintable(grabber->failure()));

    const QRect before = window.insideOnScreen();
    window.moveTo(before.topLeft() + QPoint(450, 350));
    QTRY_VERIFY(!window.insideOnScreen().intersects(before));
    for (int i = 0; i < 10; i++) {
        QCOMPARE(grabUntil(*grabber, frame.get(), Result::Frame), Result::Frame);
        QCOMPARE(sizeOf(frame.get()), QSize(400, 300));
        QVERIFY(shareOf(frame.get(), ORANGE) >= NEARLY_ALL);
    }
}

void TestX11Capture::aResizedWindowIsCapturedAtItsNewSizeOnceThatHasHeld()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    TestWindow window(look(QSize(400, 300), RED));
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, 30);
    const FramePointer frame(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, frame.get(), RED), qPrintable(grabber->failure()));
    QCOMPARE(sizeOf(frame.get()), QSize(400, 300));

    QElapsedTimer sinceLastStep;
    int duringTheDrag = 0;
    for (int step = 1; step <= 10; step++) {
        window.resizeInside(QSize(400 + 20 * step, 300 + 10 * step));
        sinceLastStep.start();
        while (sinceLastStep.elapsed() < 60) {
            if (grabber->grab(frame.get(), milliseconds(20)) != Result::Frame)
                continue;
            QCOMPARE(sizeOf(frame.get()), QSize(400, 300));
            duringTheDrag++;
        }
    }
    QVERIFY(duringTheDrag > 5);

    const QSize larger(600, 400);
    while (sinceLastStep.elapsed() < 3000 && sizeOf(frame.get()) != larger) {
        if (grabber->grab(frame.get(), milliseconds(20)) == Result::Frame)
            QVERIFY(sizeOf(frame.get()) == QSize(400, 300) || sizeOf(frame.get()) == larger);
    }
    qInfo() << "larger pictures came" << sinceLastStep.elapsed() << "ms after the last step of the drag, with" << duringTheDrag << "pictures during it";
    QCOMPARE(sizeOf(frame.get()), larger);
    QVERIFY(sinceLastStep.elapsed() >= SETTLE_MS);
    QVERIFY(sinceLastStep.elapsed() < SETTLE_MS + 700);
    QVERIFY(showsWithin(*grabber, frame.get(), RED));

    const QSize smaller(300, 200);
    window.resizeInside(smaller);
    QElapsedTimer sinceResize;
    sinceResize.start();
    while (sinceResize.elapsed() < 3000 && sizeOf(frame.get()) != smaller) {
        if (grabber->grab(frame.get(), milliseconds(20)) == Result::Frame)
            QVERIFY(sizeOf(frame.get()) == larger || sizeOf(frame.get()) == smaller);
    }
    qInfo() << "smaller pictures came" << sinceResize.elapsed() << "ms after the window was made smaller";
    QCOMPARE(sizeOf(frame.get()), smaller);
    QVERIFY(sinceResize.elapsed() >= SETTLE_MS);
    QVERIFY(sinceResize.elapsed() < SETTLE_MS + 700);
    QVERIFY(showsWithin(*grabber, frame.get(), RED));
    QCOMPARE(sizeOf(frame.get()), smaller);
    QCOMPARE(X11Capture::sizeOf(window.source()), smaller);
}

void TestX11Capture::aHiddenWindowIsHiddenUntilItIsShownAgain()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    TestWindow window(look(QSize(400, 300), GREEN));
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, 30);
    const FramePointer frame(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, frame.get(), GREEN), qPrintable(grabber->failure()));
    QCOMPARE(X11Capture::sizeOf(window.source()), QSize(400, 300));

    window.hide();
    QCOMPARE(grabUntil(*grabber, frame.get(), Result::Hidden), Result::Hidden);
    QCOMPARE(X11Capture::sizeOf(window.source()), QSize());
    QVERIFY(X11Capture::thumbnail(window.source(), QSize(160, 90)).isNull());

    QElapsedTimer timer;
    timer.start();
    QCOMPARE(grabber->grab(frame.get(), milliseconds(0)), Result::Hidden);
    QVERIFY(timer.elapsed() < 20);
    timer.start();
    QCOMPARE(grabber->grab(frame.get(), milliseconds(1000)), Result::Hidden);
    QVERIFY(timer.elapsed() < 200);

    window.show();
    QVERIFY(showsWithin(*grabber, frame.get(), GREEN));
    QCOMPARE(sizeOf(frame.get()), QSize(400, 300));
}

void TestX11Capture::aMinimisedWindowIsHiddenUntilItIsRestored()
{
    if (!connection)
        QSKIP(NO_DISPLAY);
    if (!aWindowManagerRuns())
        QSKIP(NO_WINDOW_MANAGER);

    const TestWindow window(look(QSize(400, 300), ORANGE));
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, 30);
    const FramePointer frame(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, frame.get(), ORANGE), qPrintable(grabber->failure()));

    X11Capture::restoreIfMinimised(window.source());
    QCOMPARE(grabUntil(*grabber, frame.get(), Result::Frame), Result::Frame);

    TestWindow::minimise(*display, window.handle());
    QCOMPARE(grabUntil(*grabber, frame.get(), Result::Hidden), Result::Hidden);
    QCOMPARE(X11Capture::sizeOf(window.source()), QSize());
    for (int i = 0; i < 5; i++)
        QCOMPARE(grabber->grab(frame.get(), milliseconds(100)), Result::Hidden);

    X11Capture::restoreIfMinimised(window.source());
    QVERIFY(showsWithin(*grabber, frame.get(), ORANGE));
    QCOMPARE(X11Capture::sizeOf(window.source()), QSize(400, 300));
}

void TestX11Capture::aClosedWindowEndsItsCapture()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    auto window = std::make_unique<TestWindow>(look(QSize(400, 300), BLUE));
    const CaptureSource source = window->source();
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(source.id, 30);
    const FramePointer frame(av_frame_alloc());
    QVERIFY2(showsWithin(*grabber, frame.get(), BLUE), qPrintable(grabber->failure()));

    window.reset();
    QCOMPARE(grabUntil(*grabber, frame.get(), Result::Closed), Result::Closed);
    QCOMPARE(grabber->grab(frame.get(), milliseconds(100)), Result::Closed);
    QVERIFY(grabber->failure().isEmpty());
    QCOMPARE(X11Capture::sizeOf(source), QSize());
    QVERIFY(X11Capture::thumbnail(source, QSize(160, 90)).isNull());
}

void TestX11Capture::aSourceThatIsNotThereFails_data()
{
    QTest::addColumn<QString>("id");
    QTest::newRow("a screen that is not attached") << QStringLiteral("screen:NOT-ATTACHED-1");
    QTest::newRow("a screen by its old number") << QStringLiteral("screen:0");
    QTest::newRow("a screen without a name") << QStringLiteral("screen:");
    QTest::newRow("a window that was never opened") << (connection ? QStringLiteral("window:%1").arg(xcb_generate_id(connection)) : QString());
    QTest::newRow("no window") << QStringLiteral("window:0");
    QTest::newRow("neither") << QStringLiteral("pipewire:1");
    QTest::newRow("nothing") << QString();
}

void TestX11Capture::aSourceThatIsNotThereFails()
{
    if (!connection)
        QSKIP(NO_DISPLAY);
    QFETCH(QString, id);

    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(id, 30);
    QVERIFY(grabber);
    const FramePointer frame(av_frame_alloc());
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(grabber->grab(frame.get(), milliseconds(1000)), Result::Failed);
    QVERIFY(timer.elapsed() < 100);
    QCOMPARE(grabber->grab(frame.get(), milliseconds(0)), Result::Failed);
    qInfo() << "failed with:" << grabber->failure();
    QVERIFY(!grabber->failure().isEmpty());

    CaptureSource source;
    source.id = id;
    source.kind = CaptureSource::Kind::Window;
    QVERIFY(X11Capture::thumbnail(source, QSize(160, 90)).isNull());
    QCOMPARE(X11Capture::sizeOf(source), QSize());
    X11Capture::restoreIfMinimised(source);
}

void TestX11Capture::aThumbnailHasTheWindowsColour()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    const TestWindow window(look(QSize(400, 300), ORANGE));
    QTRY_VERIFY(window.isShown());

    const QImage shrunk = X11Capture::thumbnail(window.source(), QSize(160, 90));
    QVERIFY(!shrunk.isNull());
    QCOMPARE(shrunk.size(), QSize(120, 90));
    QCOMPARE(shrunk.format(), QImage::Format_RGB32);
    // QImage::pixel() calls a pixel of this format opaque whatever its fourth byte holds
    int same = 0;
    for (int y = 0; y < shrunk.height(); y++) {
        const auto *row = reinterpret_cast<const quint32 *>(shrunk.constScanLine(y));
        for (int x = 0; x < shrunk.width(); x++)
            same += row[x] == (0xFF000000u | ORANGE);
    }
    QVERIFY2(same >= NEARLY_ALL * shrunk.width() * shrunk.height(), qPrintable(QStringLiteral("%1 of %2 pixels").arg(same).arg(shrunk.width() * shrunk.height())));

    const QImage whole = X11Capture::thumbnail(window.source(), QSize(1000, 1000));
    QCOMPARE(whole.size(), QSize(400, 300));
    QCOMPARE(whole.format(), QImage::Format_RGB32);
    QCOMPARE(reinterpret_cast<const quint32 *>(whole.constScanLine(10))[10], 0xFF000000u | ORANGE);
    QVERIFY(X11Capture::thumbnail(window.source(), QSize()).isNull());

    const QImage ofTheScreen = X11Capture::thumbnail(quietScreen, QSize(160, 90));
    QVERIFY(!ofTheScreen.isNull());
    QCOMPARE(ofTheScreen.size(), quietScreen.geometry.size().scaled(QSize(160, 90), Qt::KeepAspectRatio));
    QCOMPARE(*reinterpret_cast<const quint32 *>(ofTheScreen.constScanLine(0)) >> 24, 255u);
}

void TestX11Capture::aGrabWaitsNoLongerThanItWasAskedTo()
{
    if (!connection)
        QSKIP(NO_DISPLAY);

    const TestWindow window(look(QSize(400, 300), GREEN));
    QTRY_VERIFY(window.isShown());
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, 10);
    const FramePointer frame(av_frame_alloc());

    QElapsedTimer timer;
    timer.start();
    QCOMPARE(grabber->grab(frame.get(), milliseconds(0)), Result::NoFrame);
    QVERIFY2(timer.elapsed() < 20, qPrintable(QString::number(timer.elapsed())));

    timer.start();
    QCOMPARE(grabber->grab(frame.get(), milliseconds(1000)), Result::Frame);
    QVERIFY2(timer.elapsed() < 150, qPrintable(QString::number(timer.elapsed())));

    timer.start();
    QCOMPARE(grabber->grab(frame.get(), milliseconds(30)), Result::NoFrame);
    QVERIFY2(timer.elapsed() >= 30 && timer.elapsed() < 50, qPrintable(QString::number(timer.elapsed())));

    timer.start();
    QCOMPARE(grabber->grab(frame.get(), milliseconds(1000)), Result::Frame);
    QVERIFY2(timer.elapsed() < 100, qPrintable(QString::number(timer.elapsed())));

    QTest::qSleep(500);
    timer.start();
    QCOMPARE(grabber->grab(frame.get(), milliseconds(0)), Result::Frame);
    QVERIFY2(timer.elapsed() < 20, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(grabber->grab(frame.get(), milliseconds(0)), Result::NoFrame);
    QCOMPARE(grabber->grab(frame.get(), milliseconds(1000)), Result::Frame);
    QVERIFY2(timer.elapsed() >= 95 && timer.elapsed() < 150, qPrintable(QString::number(timer.elapsed())));
}

void TestX11Capture::theFrameRateAskedForIsKept_data()
{
    QTest::addColumn<int>("framerate");
    QTest::newRow("15") << 15;
    QTest::newRow("30") << 30;
    QTest::newRow("60") << 60;
}

void TestX11Capture::theFrameRateAskedForIsKept()
{
    if (!connection)
        QSKIP(NO_DISPLAY);
    QFETCH(int, framerate);

    const TestWindow window(look(QSize(640, 360), BLUE));
    const std::unique_ptr<LinuxGrabber> grabber = X11Capture::open(window.source().id, framerate);
    const Pace pace = paceOf(*grabber, 2000);
    const double fps = pace.pictures / pace.seconds;
    qInfo() << fps << "pictures a second";
    QVERIFY(fps > framerate * 0.95 && fps < framerate * 1.05);
}

#else

class TestX11Capture : public QObject
{
    Q_OBJECT
private slots:
    void aBuildWithoutXcbCapturesNothing()
    {
        QVERIFY(!X11Capture::usable());
        QVERIFY(X11Capture::screens().isEmpty());
        QVERIFY(X11Capture::windows().isEmpty());
        QVERIFY(!X11Capture::open(QStringLiteral("screen:0"), 30));
    }
};

#endif

QTEST_GUILESS_MAIN(TestX11Capture)
#include "tst_X11Capture.moc"
