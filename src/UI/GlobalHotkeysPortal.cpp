#include "GlobalHotkeysPortal.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>

namespace {

const QString PortalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString PortalPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString ShortcutsInterface = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");
const QString RequestInterface = QStringLiteral("org.freedesktop.portal.Request");
const QString SessionInterface = QStringLiteral("org.freedesktop.portal.Session");

// a(sa{sv}), the shortcut list BindShortcuts takes
struct PortalShortcut
{
    QString id;
    QVariantMap properties;
};
using PortalShortcutList = QList<PortalShortcut>;

QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcut &shortcut)
{
    argument.beginStructure();
    argument << shortcut.id << shortcut.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcut &shortcut)
{
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.properties;
    argument.endStructure();
    return argument;
}

} // namespace

Q_DECLARE_METATYPE(PortalShortcut)
Q_DECLARE_METATYPE(PortalShortcutList)

namespace Acheron {
namespace UI {

GlobalHotkeysPortal::GlobalHotkeysPortal(QObject *parent) : QObject(parent)
{
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<PortalShortcutList>();
    QDBusConnection::sessionBus().connect(PortalService, PortalPath, ShortcutsInterface, QStringLiteral("Activated"), this,
                                          SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
}

GlobalHotkeysPortal::~GlobalHotkeysPortal()
{
    closeSession();
}

bool GlobalHotkeysPortal::available()
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    return bus.isConnected() && bus.interface() && bus.interface()->isServiceRegistered(PortalService).value();
}

void GlobalHotkeysPortal::bind(const QList<Shortcut> &shortcuts)
{
    wanted = shortcuts;
    // a session binds once, so new keys need a new session
    closeSession();
    if (!wanted.isEmpty())
        createSession();
}

QString GlobalHotkeysPortal::requestPath(const QString &token) const
{
    QString sender = QDBusConnection::sessionBus().baseService();
    sender.remove(0, 1); // the leading ':'
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
}

QString GlobalHotkeysPortal::nextToken()
{
    return QStringLiteral("acheron_hotkeys_%1").arg(++tokenCounter);
}

void GlobalHotkeysPortal::createSession()
{
    if (creatingSession)
        return;
    creatingSession = true;

    const QString token = nextToken();
    // subscribe before calling, so a fast reply can't be missed
    QDBusConnection::sessionBus().connect(PortalService, requestPath(token), RequestInterface, QStringLiteral("Response"), this,
                                          SLOT(onSessionCreated(uint, QVariantMap)));

    QDBusMessage call = QDBusMessage::createMethodCall(PortalService, PortalPath, ShortcutsInterface, QStringLiteral("CreateSession"));
    call << QVariantMap{
        { QStringLiteral("handle_token"), token },
        { QStringLiteral("session_handle_token"), nextToken() },
    };
    QDBusConnection::sessionBus().asyncCall(call);
}

void GlobalHotkeysPortal::onSessionCreated(uint response, const QVariantMap &results)
{
    creatingSession = false;
    if (response != 0)
        return; // refused or unsupported; the window's own shortcuts still work
    sessionPath = results.value(QStringLiteral("session_handle")).toString();
    if (!sessionPath.isEmpty())
        sendBind();
}

void GlobalHotkeysPortal::sendBind()
{
    PortalShortcutList list;
    for (const Shortcut &shortcut : std::as_const(wanted)) {
        QVariantMap properties{ { QStringLiteral("description"), shortcut.description } };
        if (!shortcut.preferredTrigger.isEmpty())
            properties.insert(QStringLiteral("preferred_trigger"), shortcut.preferredTrigger);
        list.append({ shortcut.id, properties });
    }

    const QString token = nextToken();
    QDBusConnection::sessionBus().connect(PortalService, requestPath(token), RequestInterface, QStringLiteral("Response"), this,
                                          SLOT(onBound(uint, QVariantMap)));

    QDBusMessage call = QDBusMessage::createMethodCall(PortalService, PortalPath, ShortcutsInterface, QStringLiteral("BindShortcuts"));
    call << QVariant::fromValue(QDBusObjectPath(sessionPath)) << QVariant::fromValue(list) << QString()
         << QVariantMap{ { QStringLiteral("handle_token"), token } };
    QDBusConnection::sessionBus().asyncCall(call);
}

void GlobalHotkeysPortal::onBound(uint, const QVariantMap &)
{
    // nothing to keep: presses arrive as Activated, whatever keys the user settled on
}

void GlobalHotkeysPortal::onActivated(const QDBusObjectPath &session, const QString &id, qulonglong, const QVariantMap &)
{
    if (!sessionPath.isEmpty() && session.path() == sessionPath)
        emit activated(id);
}

void GlobalHotkeysPortal::closeSession()
{
    if (sessionPath.isEmpty())
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(PortalService, sessionPath, SessionInterface, QStringLiteral("Close"));
    QDBusConnection::sessionBus().asyncCall(call);
    sessionPath.clear();
}

} // namespace UI
} // namespace Acheron
