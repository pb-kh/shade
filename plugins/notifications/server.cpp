#include "server.h"

#include "model.h"

#include <QDBusConnectionInterface>
#include <QDebug>

namespace notif
{

namespace
{
const QString kName = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");
} // namespace

Server::Server(Model *model, QDBusConnection bus, QObject *parent)
    : QObject(parent)
    , m_model(model)
    , m_bus(bus)
{
}

Server::~Server()
{
    if (m_running) {
        m_bus.unregisterObject(kPath);
        m_bus.unregisterService(kName);
    }
}

bool Server::start(QString *error)
{
    if (!m_bus.isConnected()) {
        if (error) *error = QStringLiteral("no session bus");
        return false;
    }
    if (!m_bus.registerObject(kPath, this, QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals)) {
        if (error) *error = QStringLiteral("cannot register %1").arg(kPath);
        return false;
    }
    // DoNotQueue: never wait in line behind another daemon.
    const auto reply = m_bus.interface()->registerService(kName, QDBusConnectionInterface::DontQueueService,
                                                          QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid() || reply.value() != QDBusConnectionInterface::ServiceRegistered) {
        m_bus.unregisterObject(kPath);
        if (error) {
            *error = reply.isValid() ? QStringLiteral("%1 is owned by another notification daemon").arg(kName)
                                     : reply.error().message();
        }
        return false;
    }
    m_running = true;
    return true;
}

quint32 Server::Notify(const QString &app, quint32 replacesId, const QString &icon, const QString &summary, const QString &body,
                       const QStringList &actions, const QVariantMap &hints, qint32 expireTimeout)
{
    Notification n;
    n.app = app;
    n.icon = icon;
    n.summary = summary;
    n.body = body;
    n.actions = actions.size() % 2 == 0 ? actions : actions.mid(0, actions.size() - 1);
    n.expireTimeout = expireTimeout;
    bool ok = false;
    const uint urgency = hints.value(QStringLiteral("urgency")).toUInt(&ok);
    n.urgency = ok && urgency <= 2 ? quint8(urgency) : quint8(Normal);
    const bool replacing = replacesId != 0 && m_model->find(replacesId);
    const quint32 id = m_model->upsert(n, replacing ? replacesId : 0);
    qInfo().noquote() << "notifications: accepted id" << id << "from" << (app.isEmpty() ? QStringLiteral("?") : app);
    emit notified(id, replacing);
    return id;
}

void Server::CloseNotification(quint32 id)
{
    // Unknown ids succeed silently (spec).
    if (m_model->remove(id)) emit NotificationClosed(id, 3);
}

QStringList Server::GetCapabilities() { return {QStringLiteral("actions"), QStringLiteral("body")}; }

QString Server::GetServerInformation(QString &vendor, QString &version, QString &specVersion)
{
    vendor = QStringLiteral("shade project");
    version = QStringLiteral("2.0.0");
    specVersion = QStringLiteral("1.2");
    return QStringLiteral("shade");
}

void Server::dismiss(quint32 id, bool invokeDefault)
{
    const Notification *n = m_model->find(id);
    if (!n) return;
    const QString action = n->defaultAction();
    if (invokeDefault && !action.isEmpty()) emit ActionInvoked(id, action);
    if (m_model->remove(id)) emit NotificationClosed(id, 2);
}

void Server::clearAll()
{
    for (quint32 id : m_model->clear()) emit NotificationClosed(id, 2);
}

} // namespace notif
