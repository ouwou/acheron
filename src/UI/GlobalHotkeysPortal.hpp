#pragma once

#include <QDBusObjectPath>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace Acheron {
namespace UI {

// Wayland's way to global hotkeys: the org.freedesktop.portal.GlobalShortcuts portal (KDE Plasma,
// GNOME 48+, and others). Acheron proposes keys; the desktop may ask the user to confirm or
// change them and then reports presses with the shortcut's id.
class GlobalHotkeysPortal : public QObject
{
    Q_OBJECT
public:
    struct Shortcut
    {
        QString id;
        QString description;
        QString preferredTrigger; // e.g. CTRL+SHIFT+m, or empty to let the user choose
    };

    explicit GlobalHotkeysPortal(QObject *parent = nullptr);
    ~GlobalHotkeysPortal() override;

    [[nodiscard]] static bool available();
    void bind(const QList<Shortcut> &shortcuts); // replaces what was bound before

signals:
    void activated(const QString &id);

private slots:
    void onSessionCreated(uint response, const QVariantMap &results);
    void onBound(uint response, const QVariantMap &results);
    void onActivated(const QDBusObjectPath &session, const QString &id, qulonglong timestamp, const QVariantMap &options);

private:
    QString requestPath(const QString &token) const;
    QString nextToken();
    void createSession();
    void sendBind();
    void closeSession();

    QString sessionPath;
    bool creatingSession = false;
    QList<Shortcut> wanted;
    int tokenCounter = 0;
};

} // namespace UI
} // namespace Acheron
