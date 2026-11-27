#pragma once

#include <QObject>

#include <functional>

class QEvent;

namespace Acheron {
namespace UI {

// How long the user has been away from the keyboard and mouse, system-wide where the platform
// says (Windows GetLastInputInfo, macOS CGEventSource, GNOME/KDE over D-Bus), otherwise judged
// from input to Acheron's own windows. Answers when asked; it never polls on its own.
class IdleMonitor : public QObject
{
    Q_OBJECT
public:
    explicit IdleMonitor(QObject *parent = nullptr);
    ~IdleMonitor() override;

    // the callback may run later (D-Bus answers asynchronously)
    void query(std::function<void(qint64 idleMs)> callback);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void useWindowActivity();
    [[nodiscard]] qint64 windowIdleMs() const;

    bool watchingWindows = false;
    qint64 lastInputMs = 0;
};

} // namespace UI
} // namespace Acheron
