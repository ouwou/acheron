#include "GlobalHotkeys.hpp"

#include <QCoreApplication>
#include <QGuiApplication>

#include <optional>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <Carbon/Carbon.h>
#else
#if defined(ACHERON_HOTKEYS_XCB) && QT_VERSION >= QT_VERSION_CHECK(6, 2, 0) && defined(QT_FEATURE_xcb) && QT_FEATURE_xcb == 1
#define ACHERON_HOTKEYS_X11
#include <xcb/xcb.h>
#include <cstdlib>
#endif
#ifdef ACHERON_HOTKEYS_PORTAL
#include "GlobalHotkeysPortal.hpp"
#endif
#endif

namespace Acheron {
namespace UI {

namespace {

struct KeyParts
{
    int key = 0;
    Qt::KeyboardModifiers modifiers;
};

std::optional<KeyParts> splitKey(const QKeySequence &sequence)
{
    if (sequence.isEmpty())
        return std::nullopt;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QKeyCombination combination = sequence[0];
    KeyParts parts{ int(combination.key()), combination.keyboardModifiers() };
#else
    const int combined = sequence[0];
    KeyParts parts{ combined & ~int(Qt::KeyboardModifierMask), Qt::KeyboardModifiers(combined & int(Qt::KeyboardModifierMask)) };
#endif
    // a bare letter or digit grabbed system-wide would stop it from being typed anywhere
    const bool functionKey = parts.key >= Qt::Key_F1 && parts.key <= Qt::Key_F35;
    const bool hasModifier = parts.modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (!functionKey && !hasModifier && parts.key != Qt::Key_Pause && parts.key != Qt::Key_ScrollLock)
        return std::nullopt;
    return parts;
}

} // namespace

#if defined(Q_OS_WIN)

namespace {

UINT virtualKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return 'A' + (key - Qt::Key_A);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return '0' + (key - Qt::Key_0);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return VK_F1 + (key - Qt::Key_F1);
    switch (key) {
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Insert: return VK_INSERT;
    case Qt::Key_Delete: return VK_DELETE;
    case Qt::Key_Home: return VK_HOME;
    case Qt::Key_End: return VK_END;
    case Qt::Key_PageUp: return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_Pause: return VK_PAUSE;
    case Qt::Key_Print: return VK_SNAPSHOT;
    case Qt::Key_ScrollLock: return VK_SCROLL;
    case Qt::Key_Escape: return VK_ESCAPE;
    case Qt::Key_Tab: return VK_TAB;
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Return:
    case Qt::Key_Enter: return VK_RETURN;
    default: break;
    }
    if (key >= 0x20 && key < 0x7f) { // punctuation, on whatever layout is active
        const SHORT scanned = VkKeyScanW(WCHAR(key));
        if (scanned != -1)
            return LOBYTE(scanned);
    }
    return 0;
}

} // namespace

struct GlobalHotkeys::Platform
{
    std::array<bool, ActionCount> registered{};

    bool add(int action, const KeyParts &parts)
    {
        const UINT vk = virtualKey(parts.key);
        if (!vk)
            return false;
        UINT modifiers = MOD_NOREPEAT;
        if (parts.modifiers & Qt::ControlModifier)
            modifiers |= MOD_CONTROL;
        if (parts.modifiers & Qt::ShiftModifier)
            modifiers |= MOD_SHIFT;
        if (parts.modifiers & Qt::AltModifier)
            modifiers |= MOD_ALT;
        if (parts.modifiers & Qt::MetaModifier)
            modifiers |= MOD_WIN;
        // no window: WM_HOTKEY goes to this (the GUI) thread's queue
        registered[action] = RegisterHotKey(nullptr, action + 1, modifiers, vk);
        return registered[action];
    }

    void remove(int action)
    {
        if (registered[action])
            UnregisterHotKey(nullptr, action + 1);
        registered[action] = false;
    }

    bool filter(GlobalHotkeys *self, const QByteArray &eventType, void *message)
    {
        if (eventType != "windows_generic_MSG")
            return false;
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->message != WM_HOTKEY || msg->hwnd)
            return false;
        const int action = int(msg->wParam) - 1;
        if (action < 0 || action >= ActionCount || !registered[action])
            return false;
        self->trigger(action);
        return true;
    }

    void setAll(GlobalHotkeys *, const std::array<QKeySequence, ActionCount> &keys)
    {
        for (int action = 0; action < ActionCount; ++action) {
            remove(action);
            if (auto parts = splitKey(keys[action]))
                add(action, *parts);
        }
    }
};

#elif defined(Q_OS_MACOS)

namespace {

constexpr OSType HotkeySignature = 0x41434852; // 'ACHR'

int macKeyCode(int key)
{
    static const int letters[] = {
        kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I,
        kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R,
        kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z,
    };
    static const int digits[] = {
        kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4, kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9,
    };
    static const int functions[] = {
        kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10,
        kVK_F11, kVK_F12, kVK_F13, kVK_F14, kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20,
    };
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return letters[key - Qt::Key_A];
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return digits[key - Qt::Key_0];
    if (key >= Qt::Key_F1 && key <= Qt::Key_F20)
        return functions[key - Qt::Key_F1];
    switch (key) {
    case Qt::Key_Space: return kVK_Space;
    case Qt::Key_Delete: return kVK_ForwardDelete;
    case Qt::Key_Home: return kVK_Home;
    case Qt::Key_End: return kVK_End;
    case Qt::Key_PageUp: return kVK_PageUp;
    case Qt::Key_PageDown: return kVK_PageDown;
    case Qt::Key_Left: return kVK_LeftArrow;
    case Qt::Key_Up: return kVK_UpArrow;
    case Qt::Key_Right: return kVK_RightArrow;
    case Qt::Key_Down: return kVK_DownArrow;
    case Qt::Key_Escape: return kVK_Escape;
    case Qt::Key_Tab: return kVK_Tab;
    case Qt::Key_Backspace: return kVK_Delete;
    case Qt::Key_Return:
    case Qt::Key_Enter: return kVK_Return;
    default: return -1;
    }
}

OSStatus onHotkeyPressed(EventHandlerCallRef, EventRef event, void *data)
{
    EventHotKeyID id;
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(id), nullptr, &id) == noErr &&
        id.signature == HotkeySignature)
        static_cast<GlobalHotkeys *>(data)->trigger(int(id.id) - 1);
    return noErr;
}

} // namespace

struct GlobalHotkeys::Platform
{
    EventHandlerRef handler = nullptr;
    std::array<EventHotKeyRef, ActionCount> refs{};

    ~Platform()
    {
        for (int action = 0; action < ActionCount; ++action)
            remove(action);
        if (handler)
            RemoveEventHandler(handler);
    }

    bool add(GlobalHotkeys *self, int action, const KeyParts &parts)
    {
        const int code = macKeyCode(parts.key);
        if (code < 0)
            return false;
        if (!handler) {
            const EventTypeSpec pressed{ kEventClassKeyboard, kEventHotKeyPressed };
            InstallEventHandler(GetApplicationEventTarget(), &onHotkeyPressed, 1, &pressed, self, &handler);
        }
        // Qt's Control is the Command key on macOS, and its Meta is Control
        UInt32 modifiers = 0;
        if (parts.modifiers & Qt::ControlModifier)
            modifiers |= cmdKey;
        if (parts.modifiers & Qt::MetaModifier)
            modifiers |= controlKey;
        if (parts.modifiers & Qt::AltModifier)
            modifiers |= optionKey;
        if (parts.modifiers & Qt::ShiftModifier)
            modifiers |= shiftKey;
        const EventHotKeyID id{ HotkeySignature, UInt32(action + 1) };
        return RegisterEventHotKey(UInt32(code), modifiers, id, GetApplicationEventTarget(), 0, &refs[action]) == noErr;
    }

    void remove(int action)
    {
        if (refs[action])
            UnregisterEventHotKey(refs[action]);
        refs[action] = nullptr;
    }

    bool filter(GlobalHotkeys *, const QByteArray &, void *) { return false; }

    void setAll(GlobalHotkeys *self, const std::array<QKeySequence, ActionCount> &keys)
    {
        for (int action = 0; action < ActionCount; ++action) {
            remove(action);
            if (auto parts = splitKey(keys[action]))
                add(self, action, *parts);
        }
    }
};

#else // X11 and Wayland

namespace {

// the X keysym for a Qt key (Latin-1 keysyms equal their character)
quint32 keysymFor(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return 'a' + (key - Qt::Key_A);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        return 0xffbe + (key - Qt::Key_F1);
    switch (key) {
    case Qt::Key_Space: return 0x0020;
    case Qt::Key_Insert: return 0xff63;
    case Qt::Key_Delete: return 0xffff;
    case Qt::Key_Home: return 0xff50;
    case Qt::Key_End: return 0xff57;
    case Qt::Key_PageUp: return 0xff55;
    case Qt::Key_PageDown: return 0xff56;
    case Qt::Key_Left: return 0xff51;
    case Qt::Key_Up: return 0xff52;
    case Qt::Key_Right: return 0xff53;
    case Qt::Key_Down: return 0xff54;
    case Qt::Key_Pause: return 0xff13;
    case Qt::Key_Print: return 0xff61;
    case Qt::Key_ScrollLock: return 0xff14;
    case Qt::Key_Escape: return 0xff1b;
    case Qt::Key_Tab: return 0xff09;
    case Qt::Key_Backspace: return 0xff08;
    case Qt::Key_Return:
    case Qt::Key_Enter: return 0xff0d;
    default: break;
    }
    if (key > 0x20 && key < 0x7f)
        return quint32(key);
    return 0;
}

#ifdef ACHERON_HOTKEYS_PORTAL
// the shortcut spec's trigger text the portal takes, e.g. CTRL+SHIFT+m
QString portalTrigger(const QKeySequence &sequence)
{
    auto parts = splitKey(sequence);
    if (!parts)
        return {};
    QString name;
    if (parts->key >= Qt::Key_A && parts->key <= Qt::Key_Z)
        name = QChar('a' + (parts->key - Qt::Key_A));
    else if (parts->key >= Qt::Key_0 && parts->key <= Qt::Key_9)
        name = QChar('0' + (parts->key - Qt::Key_0));
    else if (parts->key >= Qt::Key_F1 && parts->key <= Qt::Key_F35)
        name = QStringLiteral("F%1").arg(parts->key - Qt::Key_F1 + 1);
    else
        return {}; // let the desktop ask
    QStringList pieces;
    if (parts->modifiers & Qt::ControlModifier)
        pieces << QStringLiteral("CTRL");
    if (parts->modifiers & Qt::AltModifier)
        pieces << QStringLiteral("ALT");
    if (parts->modifiers & Qt::ShiftModifier)
        pieces << QStringLiteral("SHIFT");
    if (parts->modifiers & Qt::MetaModifier)
        pieces << QStringLiteral("LOGO");
    pieces << name;
    return pieces.join(QLatin1Char('+'));
}
#endif

} // namespace

struct GlobalHotkeys::Platform
{
#ifdef ACHERON_HOTKEYS_X11
    static constexpr uint16_t ModifierMask = XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4;
    // grabs are exact, so each is repeated with Caps Lock and Num Lock on
    static constexpr uint16_t LockVariants[] = { 0, XCB_MOD_MASK_LOCK, XCB_MOD_MASK_2, XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2 };

    struct Grab
    {
        xcb_keycode_t keycode = 0;
        uint16_t modifiers = 0;
    };
    xcb_connection_t *connection = nullptr;
    xcb_window_t root = 0;
    std::array<Grab, ActionCount> grabs{};
#endif
#ifdef ACHERON_HOTKEYS_PORTAL
    GlobalHotkeysPortal *portal = nullptr;
#endif

    explicit Platform(GlobalHotkeys *self)
    {
        const QString platformName = QGuiApplication::platformName();
#ifdef ACHERON_HOTKEYS_X11
        if (platformName == QLatin1String("xcb")) {
            if (auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
                connection = x11->connection();
                if (connection)
                    root = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
            }
        }
#endif
#ifdef ACHERON_HOTKEYS_PORTAL
        if (platformName.startsWith(QLatin1String("wayland")) && GlobalHotkeysPortal::available()) {
            portal = new GlobalHotkeysPortal(self);
            QObject::connect(portal, &GlobalHotkeysPortal::activated, self, [self](const QString &id) {
                self->trigger(id == QLatin1String("toggle-deafen") ? int(Action::ToggleDeafen) : int(Action::ToggleMute));
            });
        }
#endif
        Q_UNUSED(self);
        Q_UNUSED(platformName);
    }

    ~Platform()
    {
#ifdef ACHERON_HOTKEYS_X11
        for (int action = 0; action < ActionCount; ++action)
            ungrab(action);
#endif
    }

#ifdef ACHERON_HOTKEYS_X11
    xcb_keycode_t keycodeFor(quint32 keysym) const
    {
        const xcb_setup_t *setup = xcb_get_setup(connection);
        const int count = setup->max_keycode - setup->min_keycode + 1;
        xcb_get_keyboard_mapping_reply_t *reply = xcb_get_keyboard_mapping_reply(
                connection, xcb_get_keyboard_mapping(connection, setup->min_keycode, uint8_t(count)), nullptr);
        if (!reply)
            return 0;
        xcb_keycode_t found = 0;
        const xcb_keysym_t *keysyms = xcb_get_keyboard_mapping_keysyms(reply);
        const int length = xcb_get_keyboard_mapping_keysyms_length(reply);
        const int perKeycode = qMax(1, int(reply->keysyms_per_keycode));
        for (int i = 0; i < length; ++i) {
            if (keysyms[i] == keysym) {
                found = xcb_keycode_t(setup->min_keycode + i / perKeycode);
                break;
            }
        }
        std::free(reply);
        return found;
    }

    bool grab(int action, const KeyParts &parts)
    {
        const quint32 keysym = keysymFor(parts.key);
        const xcb_keycode_t keycode = keysym ? keycodeFor(keysym) : 0;
        if (!keycode)
            return false;
        uint16_t modifiers = 0;
        if (parts.modifiers & Qt::ShiftModifier)
            modifiers |= XCB_MOD_MASK_SHIFT;
        if (parts.modifiers & Qt::ControlModifier)
            modifiers |= XCB_MOD_MASK_CONTROL;
        if (parts.modifiers & Qt::AltModifier)
            modifiers |= XCB_MOD_MASK_1;
        if (parts.modifiers & Qt::MetaModifier)
            modifiers |= XCB_MOD_MASK_4;

        bool ok = true;
        for (uint16_t variant : LockVariants) {
            xcb_void_cookie_t cookie = xcb_grab_key_checked(connection, 1, root, modifiers | variant, keycode,
                                                            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
            if (xcb_generic_error_t *error = xcb_request_check(connection, cookie)) {
                std::free(error);
                ok = false; // another program holds it
            }
        }
        grabs[action] = { keycode, modifiers };
        if (!ok)
            ungrab(action);
        return ok;
    }

    void ungrab(int action)
    {
        if (!connection || !grabs[action].keycode)
            return;
        for (uint16_t variant : LockVariants)
            xcb_ungrab_key(connection, grabs[action].keycode, root, grabs[action].modifiers | variant);
        xcb_flush(connection);
        grabs[action] = {};
    }
#endif

    bool filter(GlobalHotkeys *self, const QByteArray &eventType, void *message)
    {
#ifdef ACHERON_HOTKEYS_X11
        if (!connection || eventType != "xcb_generic_event_t")
            return false;
        const auto *event = static_cast<const xcb_generic_event_t *>(message);
        if ((event->response_type & ~0x80) != XCB_KEY_PRESS)
            return false;
        const auto *press = reinterpret_cast<const xcb_key_press_event_t *>(event);
        for (int action = 0; action < ActionCount; ++action) {
            if (grabs[action].keycode && press->detail == grabs[action].keycode &&
                (press->state & ModifierMask) == grabs[action].modifiers) {
                self->trigger(action);
                return true;
            }
        }
#else
        Q_UNUSED(self);
        Q_UNUSED(eventType);
        Q_UNUSED(message);
#endif
        return false;
    }

    void setAll(GlobalHotkeys *, const std::array<QKeySequence, ActionCount> &keys)
    {
#ifdef ACHERON_HOTKEYS_X11
        if (connection) {
            for (int action = 0; action < ActionCount; ++action) {
                ungrab(action);
                if (auto parts = splitKey(keys[action]))
                    grab(action, *parts);
            }
            xcb_flush(connection);
        }
#endif
#ifdef ACHERON_HOTKEYS_PORTAL
        if (portal) {
            QList<GlobalHotkeysPortal::Shortcut> shortcuts;
            if (splitKey(keys[int(Action::ToggleMute)]))
                shortcuts.append({ QStringLiteral("toggle-mute"), GlobalHotkeys::tr("Toggle mute"), portalTrigger(keys[int(Action::ToggleMute)]) });
            if (splitKey(keys[int(Action::ToggleDeafen)]))
                shortcuts.append({ QStringLiteral("toggle-deafen"), GlobalHotkeys::tr("Toggle deafen"), portalTrigger(keys[int(Action::ToggleDeafen)]) });
            portal->bind(shortcuts);
        }
#endif
        Q_UNUSED(keys);
    }
};

#endif

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
GlobalHotkeys::GlobalHotkeys(QObject *parent) : QObject(parent), platform(std::make_unique<Platform>())
#else
GlobalHotkeys::GlobalHotkeys(QObject *parent) : QObject(parent), platform(std::make_unique<Platform>(this))
#endif
{
    QCoreApplication::instance()->installNativeEventFilter(this);
}

GlobalHotkeys::~GlobalHotkeys()
{
    QCoreApplication::instance()->removeNativeEventFilter(this);
    platform->setAll(this, {});
}

bool GlobalHotkeys::confirmedByDesktop()
{
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

void GlobalHotkeys::setHotkeys(const QKeySequence &mute, const QKeySequence &deafen)
{
    const std::array<QKeySequence, ActionCount> wanted{ mute, deafen };
    if (wanted == keys)
        return;
    keys = wanted;
    platform->setAll(this, keys);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool GlobalHotkeys::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *)
#else
bool GlobalHotkeys::nativeEventFilter(const QByteArray &eventType, void *message, long *)
#endif
{
    return platform->filter(this, eventType, message);
}

void GlobalHotkeys::trigger(int action)
{
    if (action >= 0 && action < ActionCount)
        emit activated(Action(action));
}

} // namespace UI
} // namespace Acheron
