#pragma once

#include <QAbstractNativeEventFilter>
#include <QKeySequence>
#include <QObject>

#include <array>
#include <memory>

namespace Acheron {
namespace UI {

// System-wide hotkeys that work while another program has focus, for the voice toggles.
//   Windows  RegisterHotKey, delivered as WM_HOTKEY to this thread
//   macOS    Carbon RegisterEventHotKey (needs no Accessibility permission)
//   X11      a key grab on the root window
//   Wayland  the desktop's GlobalShortcuts portal, where the desktop has the final say on keys
// Nothing polls: each platform delivers the key press as an event. A key that can't be
// registered (taken by another program, no portal) just isn't global; the window's own
// shortcut still works.
class GlobalHotkeys : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    enum class Action {
        ToggleMute,
        ToggleDeafen,
    };
    static constexpr int ActionCount = 2;

    explicit GlobalHotkeys(QObject *parent = nullptr);
    ~GlobalHotkeys() override;

    // an empty sequence removes the action's hotkey; only the first key of a sequence is used
    void setHotkeys(const QKeySequence &mute, const QKeySequence &deafen);

    // Wayland: the desktop asks the user about the keys, so they're bound once, not per call
    [[nodiscard]] static bool confirmedByDesktop();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
#else
    bool nativeEventFilter(const QByteArray &eventType, void *message, long *result) override;
#endif

    void trigger(int action); // from the platform code

signals:
    void activated(Acheron::UI::GlobalHotkeys::Action action);

private:
    struct Platform;
    std::unique_ptr<Platform> platform;
    std::array<QKeySequence, ActionCount> keys;
};

} // namespace UI
} // namespace Acheron
