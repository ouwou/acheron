#include "Core/Video/LinuxCapture.hpp"

#if defined(ACHERON_HAVE_XCB) && defined(ACHERON_HAVE_AVDEVICE)
#include "Core/Logging.hpp"
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Video/CaptureCommon.hpp"

#include <QByteArray>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

#include <xcb/composite.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
}
#endif

namespace Acheron {
namespace Core {
namespace Video {
namespace X11Capture {

#if defined(ACHERON_HAVE_XCB) && defined(ACHERON_HAVE_AVDEVICE)

namespace {

using Clock = std::chrono::steady_clock;

const QLatin1String SCREEN_ID_PREFIX("screen:");
const QLatin1String WINDOW_ID_PREFIX("window:");
// As ScreenCapture::RESIZE_SETTLE_MS has it for Windows.
constexpr auto RESIZE_SETTLE = std::chrono::milliseconds(300);
constexpr auto HIDDEN_WAIT = std::chrono::milliseconds(50);
// A twentieth of a second, for a program to draw its window again once the server keeps it apart.
constexpr int THUMBNAIL_FRAMERATE = 20;
constexpr auto THUMBNAIL_WAIT = std::chrono::milliseconds(500);
// WM_STATE's NormalState and IconicState (ICCCM 4.1.3.1).
constexpr quint32 NORMAL_STATE = 1;
constexpr quint32 ICONIC_STATE = 3;
constexpr uint32_t LONGEST_PROPERTY = 4096;
// How many windows a window manager may put around a program's. Any program can nest more.
constexpr int DEEPEST_FRAME = 4;

struct Disconnect
{
    void operator()(xcb_connection_t *connection) const { xcb_disconnect(connection); }
};
struct Free
{
    void operator()(void *reply) const { std::free(reply); }
};
template <typename T>
using Reply = std::unique_ptr<T, Free>;

using Media::FramePointer;
using Media::InputPointer;
using Media::PacketPointer;

struct Display
{
    std::unique_ptr<xcb_connection_t, Disconnect> connection;
    xcb_window_t root = XCB_NONE;

    explicit operator bool() const { return root != XCB_NONE; }
    operator xcb_connection_t *() const { return connection.get(); }
};

Display connectToDisplay()
{
    Display display;
    int number = 0;
    display.connection.reset(xcb_connect(nullptr, &number));
    if (xcb_connection_has_error(display))
        return display;

    xcb_screen_iterator_t screen = xcb_setup_roots_iterator(xcb_get_setup(display));
    for (; screen.rem > 1 && number > 0; number--)
        xcb_screen_next(&screen);
    if (screen.rem > 0)
        display.root = screen.data->root;
    return display;
}

// A request of an extension the server lacks ends the connection, so it is asked first.
bool has(const Display &display, xcb_extension_t *extension)
{
    const xcb_query_extension_reply_t *found = xcb_get_extension_data(display, extension);
    return found && found->present;
}

xcb_atom_t atom(const Display &display, const char *name)
{
    const Reply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(display, xcb_intern_atom(display, 0, static_cast<uint16_t>(std::strlen(name)), name), nullptr));
    return reply ? reply->atom : XCB_ATOM_NONE;
}

struct Atoms
{
    explicit Atoms(const Display &display) : clientListStacking(atom(display, "_NET_CLIENT_LIST_STACKING")), clientList(atom(display, "_NET_CLIENT_LIST")), wmState(atom(display, "WM_STATE")), processId(atom(display, "_NET_WM_PID")), windowType(atom(display, "_NET_WM_WINDOW_TYPE")), normalType(atom(display, "_NET_WM_WINDOW_TYPE_NORMAL")), dialogType(atom(display, "_NET_WM_WINDOW_TYPE_DIALOG")), state(atom(display, "_NET_WM_STATE")), skipTaskbar(atom(display, "_NET_WM_STATE_SKIP_TASKBAR")), name(atom(display, "_NET_WM_NAME")) {}

    xcb_atom_t clientListStacking;
    xcb_atom_t clientList;
    xcb_atom_t wmState;
    xcb_atom_t processId;
    xcb_atom_t windowType;
    xcb_atom_t normalType;
    xcb_atom_t dialogType;
    xcb_atom_t state;
    xcb_atom_t skipTaskbar;
    xcb_atom_t name;
};

Reply<xcb_get_property_reply_t> property(const Display &display, xcb_window_t window, xcb_atom_t name)
{
    return Reply<xcb_get_property_reply_t>(xcb_get_property_reply(display, xcb_get_property(display, 0, window, name, XCB_GET_PROPERTY_TYPE_ANY, 0, LONGEST_PROPERTY), nullptr));
}

std::vector<quint32> numbers(const Display &display, xcb_window_t window, xcb_atom_t name)
{
    const Reply<xcb_get_property_reply_t> reply = property(display, window, name);
    if (!reply || reply->format != 32)
        return {};
    const auto *values = static_cast<const quint32 *>(xcb_get_property_value(reply.get()));
    return std::vector<quint32>(values, values + reply->value_len);
}

QByteArray text(const Display &display, xcb_window_t window, xcb_atom_t name)
{
    const Reply<xcb_get_property_reply_t> reply = property(display, window, name);
    if (!reply || reply->format != 8)
        return {};
    return QByteArray(static_cast<const char *>(xcb_get_property_value(reply.get())), xcb_get_property_value_length(reply.get()));
}

bool holds(const std::vector<quint32> &values, quint32 value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

const AVInputFormat *grabDevice()
{
    static const AVInputFormat *const device = [] {
        avdevice_register_all();
        return av_find_input_format("x11grab");
    }();
    return device;
}

std::vector<xcb_randr_monitor_info_t> monitorsOf(const Display &display)
{
    std::vector<xcb_randr_monitor_info_t> monitors;
    if (!has(display, &xcb_randr_id))
        return monitors;

    const Reply<xcb_randr_get_monitors_reply_t> reply(xcb_randr_get_monitors_reply(display, xcb_randr_get_monitors(display, display.root, 1), nullptr));
    if (!reply)
        return monitors;
    for (xcb_randr_monitor_info_iterator_t each = xcb_randr_get_monitors_monitors_iterator(reply.get()); each.rem > 0; xcb_randr_monitor_info_next(&each))
        monitors.push_back(*each.data);
    std::sort(monitors.begin(), monitors.end(), [](const xcb_randr_monitor_info_t &one, const xcb_randr_monitor_info_t &other) {
        return std::make_tuple(!one.primary, one.x, one.y) < std::make_tuple(!other.primary, other.x, other.y);
    });
    return monitors;
}

QByteArray monitorName(const Display &display, const xcb_randr_monitor_info_t &monitor)
{
    const Reply<xcb_get_atom_name_reply_t> name(xcb_get_atom_name_reply(display, xcb_get_atom_name(display, monitor.name), nullptr));
    return name ? QByteArray(xcb_get_atom_name_name(name.get()), xcb_get_atom_name_name_length(name.get())) : QByteArray();
}

// A screen by the name of the output it is on, which stays its own while others come and go.
struct ScreenArea
{
    QString name;
    QRect area;
};

std::vector<ScreenArea> screenAreas(const Display &display)
{
    std::vector<ScreenArea> areas;
    for (const xcb_randr_monitor_info_t &monitor : monitorsOf(display))
        areas.push_back({ QString::fromLatin1(monitorName(display, monitor)), QRect(monitor.x, monitor.y, monitor.width, monitor.height) });
    if (!areas.empty())
        return areas;

    const Reply<xcb_get_geometry_reply_t> whole(xcb_get_geometry_reply(display, xcb_get_geometry(display, display.root), nullptr));
    if (whole)
        areas.push_back({ QStringLiteral("root"), QRect(0, 0, whole->width, whole->height) });
    return areas;
}

// Xwayland's root window shows nothing of the desktop. It says what it is by an extension from
// 23.1 on, and before that by the names of its outputs.
bool isXwayland(const Display &display)
{
    const Reply<xcb_query_extension_reply_t> extension(xcb_query_extension_reply(display, xcb_query_extension(display, 8, "XWAYLAND"), nullptr));
    if (extension && extension->present)
        return true;

    for (const xcb_randr_monitor_info_t &monitor : monitorsOf(display)) {
        if (monitorName(display, monitor).startsWith("XWAYLAND"))
            return true;
    }
    return false;
}

xcb_window_t windowOf(const QString &sourceId)
{
    bool numbered = false;
    const uint handle = sourceId.mid(WINDOW_ID_PREFIX.size()).toUInt(&numbered);
    return sourceId.startsWith(WINDOW_ID_PREFIX) && numbered ? handle : XCB_NONE;
}

enum class Showing {
    Gone,
    Hidden,
    Shown,
};

Showing showingOf(const Display &display, xcb_window_t window)
{
    const Reply<xcb_get_window_attributes_reply_t> attributes(xcb_get_window_attributes_reply(display, xcb_get_window_attributes(display, window), nullptr));
    if (!attributes)
        return Showing::Gone;
    return attributes->map_state == XCB_MAP_STATE_VIEWABLE ? Showing::Shown : Showing::Hidden;
}

// GTK draws a window's shadow inside the window itself, and says in _GTK_FRAME_EXTENTS how
// much of its left, right, top and bottom that is.
QRect insideOf(const Display &display, xcb_window_t window)
{
    const Reply<xcb_get_geometry_reply_t> geometry(xcb_get_geometry_reply(display, xcb_get_geometry(display, window), nullptr));
    if (!geometry)
        return {};
    const std::vector<quint32> shadow = numbers(display, window, atom(display, "_GTK_FRAME_EXTENTS"));
    const QRect whole(0, 0, geometry->width, geometry->height);
    if (shadow.size() != 4 || quint64(shadow[0]) + shadow[1] >= geometry->width || quint64(shadow[2]) + shadow[3] >= geometry->height)
        return whole;
    return whole.adjusted(static_cast<int>(shadow[0]), static_cast<int>(shadow[2]), -static_cast<int>(shadow[1]), -static_cast<int>(shadow[3]));
}

QRect insideOnRoot(const Display &display, xcb_window_t window)
{
    const Reply<xcb_translate_coordinates_reply_t> corner(xcb_translate_coordinates_reply(display, xcb_translate_coordinates(display, window, display.root, 0, 0), nullptr));
    return corner ? insideOf(display, window).translated(corner->dst_x, corner->dst_y) : QRect();
}

std::vector<xcb_window_t> childrenOf(const Display &display, xcb_window_t window)
{
    const Reply<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(display, xcb_query_tree(display, window), nullptr));
    if (!tree)
        return {};
    const xcb_window_t *children = xcb_query_tree_children(tree.get());
    return std::vector<xcb_window_t>(children, children + xcb_query_tree_children_length(tree.get()));
}

xcb_window_t clientWithin(const Display &display, const Atoms &atoms, xcb_window_t window, int framesDeep = DEEPEST_FRAME)
{
    if (!numbers(display, window, atoms.wmState).empty())
        return window;
    for (xcb_window_t child : framesDeep > 0 ? childrenOf(display, window) : std::vector<xcb_window_t>()) {
        if (const xcb_window_t client = clientWithin(display, atoms, child, framesDeep - 1))
            return client;
    }
    return XCB_NONE;
}

std::vector<xcb_window_t> topDownWindows(const Display &display, const Atoms &atoms)
{
    std::vector<xcb_window_t> windows = numbers(display, display.root, atoms.clientListStacking);
    if (windows.empty())
        windows = numbers(display, display.root, atoms.clientList);
    if (windows.empty()) {
        for (xcb_window_t child : childrenOf(display, display.root)) {
            if (const xcb_window_t client = clientWithin(display, atoms, child))
                windows.push_back(client);
        }
    }
    std::reverse(windows.begin(), windows.end());
    return windows;
}

bool isShareable(const Display &display, const Atoms &atoms, xcb_window_t window)
{
    const std::vector<quint32> state = numbers(display, window, atoms.wmState);
    if (state.empty() || (state[0] != NORMAL_STATE && state[0] != ICONIC_STATE))
        return false;
    if (holds(numbers(display, window, atoms.processId), static_cast<quint32>(getpid())))
        return false;

    const std::vector<quint32> types = numbers(display, window, atoms.windowType);
    if (!types.empty() && !holds(types, atoms.normalType) && !holds(types, atoms.dialogType))
        return false;
    return !holds(numbers(display, window, atoms.state), atoms.skipTaskbar);
}

QString titleOf(const Display &display, const Atoms &atoms, xcb_window_t window)
{
    const QByteArray title = text(display, window, atoms.name);
    return title.isEmpty() ? QString::fromLatin1(text(display, window, XCB_ATOM_WM_NAME)) : QString::fromUtf8(title);
}

using Media::describeError;

class Grabber final : public LinuxGrabber
{
public:
    Grabber(const QString &sourceId, int framerate);

    Result grab(AVFrame *into, std::chrono::milliseconds wait) override;
    [[nodiscard]] bool deliversOnlyChanges() const override { return false; }
    [[nodiscard]] QString name() const override { return QStringLiteral("x11grab"); }
    [[nodiscard]] QString failure() const override { return failureText; }

private:
    Result watchWindow(Clock::time_point until);
    int openInput();
    Result afterFailure(int error);
    Result fail(int error);
    [[nodiscard]] std::chrono::microseconds interval() const { return std::chrono::microseconds(1'000'000 / framerate); }

    Display display;
    xcb_window_t window;
    QRect screen;
    int framerate;
    InputPointer input;
    PacketPointer packet;
    // x11grab times each picture from when it was opened, and does not wait at all for a
    // reader that fell behind. This is never earlier than its time, so it does the waiting.
    Clock::time_point due;
    QRect windowArea;
    QRect pictureArea;
    Clock::time_point areaChangedAt;
    bool failedBefore = false;
    bool closed = false;
    QString failureText;
};

Grabber::Grabber(const QString &sourceId, int framerate) : display(connectToDisplay()), window(windowOf(sourceId)), framerate(std::max(1, framerate)), packet(av_packet_alloc())
{
    if (!display || !grabDevice()) {
        fail(AVERROR(EIO));
        return;
    }

    if (window) {
        if (showingOf(display, window) == Showing::Gone) {
            fail(AVERROR(ENODEV));
            return;
        }
        // Kept by the server as a picture of its own, the window is read whatever covers it.
        if (has(display, &xcb_composite_id))
            xcb_composite_redirect_window(display, window, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
        return;
    }

    for (const ScreenArea &each : screenAreas(display)) {
        if (!each.name.isEmpty() && sourceId == SCREEN_ID_PREFIX + each.name)
            screen = each.area;
    }
    if (screen.isEmpty())
        fail(AVERROR(ENODEV));
}

LinuxGrabber::Result Grabber::fail(int error)
{
    input.reset();
    const QString reason = window ? captureText("The window can no longer be captured (%1).") : captureText("The screen can no longer be captured (%1).");
    failureText = reason.arg(describeError(error));
    return Result::Failed;
}

LinuxGrabber::Result Grabber::watchWindow(Clock::time_point until)
{
    const Showing showing = showingOf(display, window);
    const QRect inside = insideOf(display, window);
    if (xcb_connection_has_error(display))
        return fail(AVERROR(EIO));
    if (showing == Showing::Gone) {
        qCInfo(LogVideo) << "The window being captured was closed";
        input.reset();
        closed = true;
        return Result::Closed;
    }
    if (showing == Showing::Hidden || inside.isEmpty()) {
        input.reset();
        pictureArea = QRect();
        failedBefore = false;
        std::this_thread::sleep_until(std::min(until, Clock::now() + HIDDEN_WAIT));
        return Result::Hidden;
    }

    const auto now = Clock::now();
    if (inside != windowArea) {
        windowArea = inside;
        areaChangedAt = now;
    }
    if (inside != pictureArea && (pictureArea.isEmpty() || now - areaChangedAt >= RESIZE_SETTLE)) {
        pictureArea = inside;
        input.reset();
    }
    // x11grab reads the part of the window it was opened with, which may no longer be all there.
    if (!inside.contains(pictureArea)) {
        std::this_thread::sleep_until(until);
        return Result::NoFrame;
    }
    return Result::Frame;
}

int Grabber::openInput()
{
    const QRect area = window ? pictureArea : screen;
    AVDictionary *options = nullptr;
    av_dict_set_int(&options, "framerate", framerate, 0);
    av_dict_set(&options, "video_size", QStringLiteral("%1x%2").arg(area.width()).arg(area.height()).toLatin1().constData(), 0);
    av_dict_set_int(&options, "grab_x", area.x(), 0);
    av_dict_set_int(&options, "grab_y", area.y(), 0);
    if (window)
        av_dict_set_int(&options, "window_id", window, 0);

    AVFormatContext *opened = nullptr;
    const int result = avformat_open_input(&opened, "", grabDevice(), &options);
    av_dict_free(&options);
    if (result < 0)
        return result;

    input.reset(opened);
    if (input->nb_streams != 1 || input->streams[0]->codecpar->format != AV_PIX_FMT_BGR0) {
        input.reset();
        return AVERROR(ENOSYS);
    }
    due = Clock::now() + interval();
    return 0;
}

LinuxGrabber::Result Grabber::afterFailure(int error)
{
    // x11grab does without shared memory for good after a read that failed. A window may
    // change between being looked at and being opened or read, once.
    qCDebug(LogVideo) << "x11grab: no picture:" << describeError(error);
    input.reset();
    return window && !std::exchange(failedBefore, true) ? Result::NoFrame : fail(error);
}

LinuxGrabber::Result Grabber::grab(AVFrame *into, std::chrono::milliseconds wait)
{
    if (!failureText.isEmpty())
        return Result::Failed;
    if (closed)
        return Result::Closed;

    const auto until = Clock::now() + wait;
    if (window) {
        const Result watched = watchWindow(until);
        if (watched != Result::Frame)
            return watched;
    }
    if (!input) {
        const int opened = openInput();
        if (opened < 0)
            return afterFailure(opened);
    }
    if (due > until) {
        std::this_thread::sleep_until(until);
        return Result::NoFrame;
    }
    std::this_thread::sleep_until(due);
    const auto now = Clock::now();
    due = (now - due > interval() ? now : due) + interval();

    const int read = av_read_frame(input.get(), packet.get());
    if (read < 0)
        return afterFailure(read);
    failedBefore = false;

    const AVCodecParameters *picture = input->streams[0]->codecpar;
    av_frame_unref(into);
    into->format = picture->format;
    into->width = picture->width;
    into->height = picture->height;
    into->pts = packet->pts;
    into->data[0] = packet->data;
    into->linesize[0] = picture->width * 4;
    into->buf[0] = std::exchange(packet->buf, nullptr);
    av_packet_unref(packet.get());
    return Result::Frame;
}

} // namespace

bool usable()
{
    static const bool answer = [] {
        const Display display = connectToDisplay();
        return display && grabDevice() && !isXwayland(display);
    }();
    return answer;
}

QList<CaptureSource> screens()
{
    QList<CaptureSource> sources;
    if (!usable())
        return sources;

    const Display display = connectToDisplay();
    if (!display)
        return sources;
    const std::vector<ScreenArea> areas = screenAreas(display);
    for (size_t i = 0; i < areas.size(); i++) {
        CaptureSource source;
        source.id = SCREEN_ID_PREFIX + areas[i].name;
        source.name = captureText("Screen %1").arg(i + 1);
        source.geometry = areas[i].area;
        sources.append(source);
    }
    return sources;
}

QList<CaptureSource> windows()
{
    QList<CaptureSource> sources;
    if (!usable())
        return sources;

    const Display display = connectToDisplay();
    if (!display)
        return sources;
    const Atoms atoms(display);
    for (xcb_window_t window : topDownWindows(display, atoms)) {
        if (!isShareable(display, atoms, window))
            continue;

        CaptureSource source;
        source.id = WINDOW_ID_PREFIX + QString::number(window);
        source.kind = CaptureSource::Kind::Window;
        source.name = titleOf(display, atoms, window);
        source.geometry = insideOnRoot(display, window);
        const std::vector<quint32> process = numbers(display, window, atoms.processId);
        source.processId = process.empty() ? 0 : process[0];
        if (!source.name.isEmpty())
            sources.append(source);
    }
    return sources;
}

QImage thumbnail(const CaptureSource &source, const QSize &bound)
{
    if (bound.isEmpty())
        return {};

    Grabber grabber(source.id, THUMBNAIL_FRAMERATE);
    const FramePointer frame(av_frame_alloc());
    if (grabber.grab(frame.get(), THUMBNAIL_WAIT) != LinuxGrabber::Result::Frame)
        return {};

    const QSize size(frame->width, frame->height);
    const QImage picture(frame->data[0], frame->width, frame->height, frame->linesize[0], QImage::Format_RGB32);
    QImage shrunk = size.width() > bound.width() || size.height() > bound.height() ? picture.scaled(size.scaled(bound, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation) : picture.copy();
    for (int y = 0; y < shrunk.height(); y++) {
        auto *row = reinterpret_cast<quint32 *>(shrunk.scanLine(y));
        // An X server leaves the byte an image keeps for opacity as it finds it.
        for (int x = 0; x < shrunk.width(); x++)
            row[x] |= 0xFF000000u;
    }
    return shrunk;
}

QSize sizeOf(const CaptureSource &source)
{
    if (source.kind == CaptureSource::Kind::Screen)
        return source.geometry.size();

    const Display display = connectToDisplay();
    const xcb_window_t window = windowOf(source.id);
    return display && window && showingOf(display, window) == Showing::Shown ? insideOf(display, window).size() : QSize();
}

void restoreIfMinimised(const CaptureSource &window)
{
    const Display display = connectToDisplay();
    const xcb_window_t handle = windowOf(window.id);
    if (!display || !handle)
        return;

    const std::vector<quint32> state = numbers(display, handle, atom(display, "WM_STATE"));
    if (state.empty() || state[0] != ICONIC_STATE)
        return;
    // A window manager takes the mapping of a window it made an icon of as asking for it back
    // (ICCCM 4.1.4). The answer is waited for: the server drops what came just before a hang-up.
    Reply<xcb_generic_error_t>(xcb_request_check(display, xcb_map_window_checked(display, handle)));
}

std::unique_ptr<LinuxGrabber> open(const QString &sourceId, int framerate)
{
    return std::make_unique<Grabber>(sourceId, framerate);
}

#else

bool usable()
{
    return false;
}

QList<CaptureSource> screens()
{
    return {};
}

QList<CaptureSource> windows()
{
    return {};
}

QImage thumbnail(const CaptureSource &, const QSize &)
{
    return {};
}

QSize sizeOf(const CaptureSource &)
{
    return {};
}

void restoreIfMinimised(const CaptureSource &)
{
}

std::unique_ptr<LinuxGrabber> open(const QString &, int)
{
    return nullptr;
}

#endif

} // namespace X11Capture
} // namespace Video
} // namespace Core
} // namespace Acheron
