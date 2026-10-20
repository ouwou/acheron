#pragma once

#include "Core/Video/CaptureSources.hpp"

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <unistd.h>

#include <xcb/xcb.h>

class X11TestWindow
{
public:
    // WM_STATE's states (ICCCM 4.1.3.1).
    static constexpr quint32 WITHDRAWN_STATE = 0;
    static constexpr quint32 NORMAL_STATE = 1;
    static constexpr quint32 ICONIC_STATE = 3;

    struct Free
    {
        void operator()(void *reply) const { std::free(reply); }
    };
    template <typename T>
    using Reply = std::unique_ptr<T, Free>;

    struct Display
    {
        Display()
        {
            int number = 0;
            connection = xcb_connect(nullptr, &number);
            if (xcb_connection_has_error(connection))
                return;
            xcb_screen_iterator_t screen = xcb_setup_roots_iterator(xcb_get_setup(connection));
            for (; screen.rem > 1 && number > 0; number--)
                xcb_screen_next(&screen);
            root = screen.data->root;
        }
        ~Display() { xcb_disconnect(connection); }

        Display(const Display &) = delete;
        Display &operator=(const Display &) = delete;

        [[nodiscard]] bool isOpen() const { return root != XCB_NONE; }

        xcb_connection_t *connection = nullptr;
        xcb_window_t root = XCB_NONE;
    };

    struct Look
    {
        QSize inside;
        QPoint at;
        quint32 colour = 0;
        bool framed = true;
        bool outOfTheWay = false;
        bool animated = false;
        QString title = QStringLiteral("Acheron capture test");
        bool ours = true;
        xcb_window_t within = XCB_NONE;
        bool shown = true;
        bool ownOpacity = false;
    };

    static xcb_atom_t atom(xcb_connection_t *connection, const char *name)
    {
        const Reply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, static_cast<uint16_t>(std::strlen(name)), name), nullptr));
        return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    }

    static std::vector<quint32> numbers(xcb_connection_t *connection, xcb_window_t window, const char *name)
    {
        const Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection, xcb_get_property(connection, 0, window, atom(connection, name), XCB_GET_PROPERTY_TYPE_ANY, 0, 4096), nullptr));
        if (!reply || reply->format != 32)
            return {};
        const auto *values = static_cast<const quint32 *>(xcb_get_property_value(reply.get()));
        return std::vector<quint32>(values, values + reply->value_len);
    }

    static QRect onRoot(const Display &display, xcb_window_t window)
    {
        const Reply<xcb_get_geometry_reply_t> geometry(xcb_get_geometry_reply(display.connection, xcb_get_geometry(display.connection, window), nullptr));
        const Reply<xcb_translate_coordinates_reply_t> corner(xcb_translate_coordinates_reply(display.connection, xcb_translate_coordinates(display.connection, window, display.root, 0, 0), nullptr));
        return geometry && corner ? QRect(corner->dst_x, corner->dst_y, geometry->width, geometry->height) : QRect();
    }

    static bool aWindowManagerRuns(const Display &display) { return !numbers(display.connection, display.root, "_NET_SUPPORTING_WM_CHECK").empty(); }

    static xcb_visualid_t visualWithOpacity(const Display &display)
    {
        for (xcb_screen_iterator_t screen = xcb_setup_roots_iterator(xcb_get_setup(display.connection)); screen.rem > 0; xcb_screen_next(&screen)) {
            if (screen.data->root != display.root)
                continue;
            for (xcb_depth_iterator_t depth = xcb_screen_allowed_depths_iterator(screen.data); depth.rem > 0; xcb_depth_next(&depth)) {
                if (depth.data->depth == 32 && xcb_depth_visuals_length(depth.data) > 0)
                    return xcb_depth_visuals(depth.data)->visual_id;
            }
        }
        return XCB_NONE;
    }

    // As a program asks the window manager to (ICCCM 4.1.4).
    static void minimise(const Display &display, xcb_window_t window)
    {
        xcb_client_message_event_t message = {};
        message.response_type = XCB_CLIENT_MESSAGE;
        message.format = 32;
        message.window = window;
        message.type = atom(display.connection, "WM_CHANGE_STATE");
        message.data.data32[0] = ICONIC_STATE;
        xcb_send_event(display.connection, 0, display.root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, reinterpret_cast<const char *>(&message));
        xcb_flush(display.connection);
    }

    explicit X11TestWindow(const Look &look) : topLevel(!look.within), managed(look.framed && !look.within), animated(look.animated)
    {
        const xcb_visualid_t visual = look.ownOpacity && display.isOpen() ? visualWithOpacity(display) : static_cast<xcb_visualid_t>(XCB_COPY_FROM_PARENT);
        if (!display.isOpen() || (look.ownOpacity && !visual))
            return;

        xcb_connection_t *connection = display.connection;
        const xcb_colormap_t colours = look.ownOpacity ? xcb_generate_id(connection) : static_cast<xcb_colormap_t>(XCB_COPY_FROM_PARENT);
        if (look.ownOpacity)
            xcb_create_colormap(connection, XCB_COLORMAP_ALLOC_NONE, colours, display.root, visual);
        // A window of another depth than the one it is in has to name its border and its colours.
        const uint32_t values[] = { look.ownOpacity ? 0xFF000000u | look.colour : look.colour, 0, !look.framed, colours };
        id = xcb_generate_id(connection);
        xcb_create_window(connection, look.ownOpacity ? 32 : XCB_COPY_FROM_PARENT, id, look.within ? look.within : display.root, static_cast<int16_t>(look.at.x()), static_cast<int16_t>(look.at.y()), static_cast<uint16_t>(look.inside.width()), static_cast<uint16_t>(look.inside.height()), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, visual, XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_COLORMAP, values);
        square = xcb_generate_id(connection);
        xcb_create_gc(connection, square, id, 0, nullptr);

        const QByteArray title = look.title.toUtf8();
        if (!title.isEmpty()) {
            xcb_change_property(connection, XCB_PROP_MODE_REPLACE, id, atom(connection, "_NET_WM_NAME"), atom(connection, "UTF8_STRING"), 8, static_cast<uint32_t>(title.size()), title.constData());
            xcb_change_property(connection, XCB_PROP_MODE_REPLACE, id, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, static_cast<uint32_t>(title.size()), title.constData());
        }
        if (look.ours)
            set("_NET_WM_PID", XCB_ATOM_CARDINAL, { static_cast<quint32>(getpid()) });
        // WM_SIZE_HINTS, whose first number says the user chose the place and the size (ICCCM 4.1.2.3).
        std::vector<quint32> hints(18, 0);
        hints[0] = 1 | 2;
        set("WM_NORMAL_HINTS", XCB_ATOM_WM_SIZE_HINTS, hints);
        if (look.outOfTheWay) {
            set("_NET_WM_STATE", XCB_ATOM_ATOM, { atom(connection, "_NET_WM_STATE_ABOVE"), atom(connection, "_NET_WM_STATE_SKIP_TASKBAR") });
            set("_NET_WM_USER_TIME", XCB_ATOM_CARDINAL, { 0 });
        }
        if (look.shown)
            show();
        ticking = std::thread([this] { tick(); });
    }

    ~X11TestWindow()
    {
        quitting = true;
        if (ticking.joinable())
            ticking.join();
    }

    X11TestWindow(const X11TestWindow &) = delete;
    X11TestWindow &operator=(const X11TestWindow &) = delete;

    [[nodiscard]] bool isOpen() const { return id != XCB_NONE; }
    [[nodiscard]] xcb_window_t handle() const { return id; }
    [[nodiscard]] QString sourceId() const { return QStringLiteral("window:%1").arg(id); }
    [[nodiscard]] Acheron::Core::Video::CaptureSource source() const
    {
        Acheron::Core::Video::CaptureSource source;
        source.id = sourceId();
        source.kind = Acheron::Core::Video::CaptureSource::Kind::Window;
        return source;
    }
    [[nodiscard]] QRect insideOnScreen() const { return onRoot(display, id); }
    [[nodiscard]] bool isShown() const
    {
        const Reply<xcb_get_window_attributes_reply_t> attributes(xcb_get_window_attributes_reply(display.connection, xcb_get_window_attributes(display.connection, id), nullptr));
        return attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE;
    }

    [[nodiscard]] bool isListed() const
    {
        const std::vector<quint32> windows = numbers(display.connection, display.root, "_NET_CLIENT_LIST");
        return std::find(windows.begin(), windows.end(), id) != windows.end();
    }

    void setAnimated(bool on) { animated = on; }

    void set(const char *name, xcb_atom_t type, const std::vector<quint32> &values)
    {
        xcb_change_property(display.connection, XCB_PROP_MODE_REPLACE, id, atom(display.connection, name), type, 32, static_cast<uint32_t>(values.size()), values.data());
        xcb_flush(display.connection);
    }
    void setAtoms(const char *name, const char *value) { set(name, XCB_ATOM_ATOM, { atom(display.connection, value) }); }
    void setState(quint32 state) { set("WM_STATE", atom(display.connection, "WM_STATE"), { state, XCB_NONE }); }

    void show()
    {
        xcb_map_window(display.connection, id);
        xcb_flush(display.connection);
        // KWin shows a window before it has put it in its place, and lists it once it has.
        const bool listed = managed && aWindowManagerRuns(display);
        for (int waited = 0; topLevel && !(isShown() && (!listed || isListed())) && waited < 5000; waited += 5)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    void hide()
    {
        xcb_unmap_window(display.connection, id);
        xcb_flush(display.connection);
    }
    void minimise() { minimise(display, id); }
    void close()
    {
        xcb_destroy_window(display.connection, id);
        xcb_flush(display.connection);
    }
    void moveTo(const QPoint &corner)
    {
        const uint32_t values[] = { static_cast<uint32_t>(corner.x()), static_cast<uint32_t>(corner.y()) };
        xcb_configure_window(display.connection, id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
        xcb_flush(display.connection);
    }
    void resizeInside(const QSize &size)
    {
        const uint32_t values[] = { static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()) };
        xcb_configure_window(display.connection, id, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
        xcb_flush(display.connection);
    }
    void paint(quint32 colour)
    {
        xcb_change_window_attributes(display.connection, id, XCB_CW_BACK_PIXEL, &colour);
        xcb_clear_area(display.connection, 0, id, 0, 0, 0, 0);
        xcb_flush(display.connection);
    }

private:
    void tick()
    {
        const xcb_rectangle_t corner = { 0, 0, 24, 24 };
        for (uint32_t number = 0; !quitting; number++) {
            if (animated) {
                const uint32_t colour = 0xFF000000u | ((number * 37 & 255) << 16) | ((number * 91 & 255) << 8) | (number * 13 & 255);
                xcb_change_gc(display.connection, square, XCB_GC_FOREGROUND, &colour);
                xcb_poly_fill_rectangle(display.connection, id, square, 1, &corner);
                xcb_flush(display.connection);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    Display display;
    const bool topLevel;
    const bool managed;
    xcb_window_t id = XCB_NONE;
    xcb_gcontext_t square = XCB_NONE;
    std::atomic<bool> animated;
    std::atomic<bool> quitting{ false };
    std::thread ticking;
};
