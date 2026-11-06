#pragma once

#include <QDBusConnection>
#include <QDBusContext>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

namespace notif
{

class Model;

// org.freedesktop.Notifications (spec 1.2) on the session bus.
class Server : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

public:
    Server(Model *model, QDBusConnection bus, QObject *parent = nullptr);
    ~Server() override;

    // Registers the object and requests the name. Fails (no fallback name)
    // when another daemon owns it.
    bool start(QString *error);
    bool running() const { return m_running; }

    // User dismissal (reason 2), optionally invoking the default action.
    void dismiss(quint32 id, bool invokeDefault);
    void clearAll();

public slots:
    Q_SCRIPTABLE quint32 Notify(const QString &app, quint32 replacesId, const QString &icon, const QString &summary,
                                const QString &body, const QStringList &actions, const QVariantMap &hints, qint32 expireTimeout);
    Q_SCRIPTABLE void CloseNotification(quint32 id);
    Q_SCRIPTABLE QStringList GetCapabilities();
    Q_SCRIPTABLE QString GetServerInformation(QString &vendor, QString &version, QString &specVersion);

signals:
    Q_SCRIPTABLE void NotificationClosed(quint32 id, quint32 reason);
    Q_SCRIPTABLE void ActionInvoked(quint32 id, const QString &actionKey);

    void notified(quint32 id, bool replaced);

private:
    Model *m_model;
    QDBusConnection m_bus;
    bool m_running = false;
};

} // namespace notif
