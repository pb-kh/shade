#pragma once

#include "model.h"
#include "server.h"

#include <shade/sdk.h>

#include <QHash>
#include <QObject>
#include <QVariantMap>

class QTimer;

namespace notif
{

// Notification daemon plugin. The daemon runs for the plugin's whole
// lifetime (activation only affects presentation). Every notification is
// also published as a peek "n-<id>" that expands the whole shade when the
// pointer reaches the top edge (expand = "top"); hovering does nothing.
class NotificationsPlugin : public QObject, public shade::Plugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ShadePlugin_iid FILE "meta.json")
    Q_INTERFACES(shade::Plugin)

public:
    bool load(shade::Host *host) override;
    void unload() override;

    Model *model() const { return m_model; }
    Server *server() const { return m_server; }
    bool daemonRunning() const { return m_server && m_server->running(); }
    QString daemonError() const { return m_error; }

    void dismiss(quint32 id, bool invokeDefault);

private:
    void publish(quint32 id);
    void withdraw(quint32 id);
    static QString peekId(quint32 id) { return QStringLiteral("n-%1").arg(id); }

    shade::Host *m_host = nullptr;
    QVariantMap m_cfg;
    Model *m_model = nullptr;
    Server *m_server = nullptr;
    QString m_error;
    QList<int> m_handlers;
};

} // namespace notif
