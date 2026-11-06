#include "plugin.h"

#include "pane.h"

#include <QDBusConnection>
#include <QDebug>

namespace notif
{

bool NotificationsPlugin::load(shade::Host *host)
{
    m_host = host;
    m_cfg = host->config();
    m_model = new Model(this);
    m_model->setDnd(m_cfg.value(QStringLiteral("do_not_disturb"), false).toBool());
    m_server = new Server(m_model, QDBusConnection::sessionBus(), this);
    if (!m_server->start(&m_error)) {
        // No fallback name: the pane explains, the rest of shade carries on.
        qWarning().noquote() << "notifications: daemon not started:" << m_error;
    } else {
        qInfo() << "notifications: acquired org.freedesktop.Notifications";
    }

    connect(m_server, &Server::notified, this, [this](quint32 id, bool) { publish(id); });
    connect(m_model, &Model::removed, this, &NotificationsPlugin::withdraw);
    connect(m_model, &Model::cleared, this, [this](const QList<quint32> &ids) {
        for (quint32 id : ids) withdraw(id);
    });
    connect(m_model, &Model::dndChanged, this, [this](bool on) {
        if (!on) return;
        for (const Notification &n : m_model->items()) withdraw(n.id);
    });

    shade::Bus *bus = host->bus();
    m_handlers << bus->handle(QString::fromLatin1(shade::sig::WidgetRequest), [this](const shade::Signal &s) {
        auto *pane = new NotificationsPane(this);
        m_host->bus()->send(QString::fromLatin1(shade::kHost), QString::fromLatin1(shade::sig::Widget),
                            QVariantMap{{QStringLiteral("widget"), QVariant::fromValue<QObject *>(pane)}}, s.window);
    });
    // Bus API for other plugins and `shadectl signal notifications ...`.
    m_handlers << bus->handle(QStringLiteral("clear"), [this](const shade::Signal &) { m_server->clearAll(); });
    m_handlers << bus->handle(QStringLiteral("dnd"), [this](const shade::Signal &s) {
        const QVariant on = s.payload.value(QStringLiteral("on"));
        m_model->setDnd(on.isValid() ? on.toBool() : !m_model->dnd());
    });
    return true;
}

void NotificationsPlugin::unload()
{
    for (int id : m_handlers) m_host->bus()->unhandle(id);
    m_handlers.clear();
    delete m_server; // releases the bus name
    m_server = nullptr;
}

void NotificationsPlugin::dismiss(quint32 id, bool invokeDefault)
{
    if (m_server) m_server->dismiss(id, invokeDefault);
}

void NotificationsPlugin::publish(quint32 id)
{
    const Notification *n = m_model->find(id);
    if (!n || m_model->dnd()) return;
    const shade::PeekPriority priority = n->urgency == Critical ? shade::PeekPriority::High
                                         : n->urgency == Low  ? shade::PeekPriority::Low
                                                              : shade::PeekPriority::Medium;
    // expire_timeout: >0 ms, 0 = never, -1 = server default (5 s).
    const int timeout = n->expireTimeout > 0 ? n->expireTimeout : n->expireTimeout == 0 ? 0
                                                                                        : m_cfg.value(QStringLiteral("peek_timeout_ms"), 5000).toInt();
    const QVariantMap data{
        {QStringLiteral("nid"), n->id},
        {QStringLiteral("app"), n->app},
        {QStringLiteral("summary"), n->summary},
        {QStringLiteral("body"), n->body},
        {QStringLiteral("urgency"), uint(n->urgency)},
        {QStringLiteral("received"), n->received.toMSecsSinceEpoch()},
    };
    m_host->bus()->send(QString::fromLatin1(shade::kHost), QString::fromLatin1(shade::sig::Peek),
                        shade::peekPayload(peekId(id), data, priority, QString::fromLatin1(shade::kExpandTop), timeout));
}

void NotificationsPlugin::withdraw(quint32 id)
{
    m_host->bus()->send(QString::fromLatin1(shade::kHost), QString::fromLatin1(shade::sig::DismissPeek),
                        QVariantMap{{QStringLiteral("id"), peekId(id)}});
}

} // namespace notif
