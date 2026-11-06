#pragma once

#include "base/pluginscan.h"
#include "core/config.h"
#include "core/control.h"
#include "core/signalbus.h"

#include <shade/sdk.h>

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <memory>

class QPluginLoader;
class QScreen;

namespace shade::app
{

class ShadeWindow;

class ShadeHost : public QObject
{
    Q_OBJECT

public:
    explicit ShadeHost(core::Config config, QObject *parent = nullptr);
    ~ShadeHost() override;

    // Load plugins, open windows, start the control socket.
    bool start(QString *error);
    void shutdown();

    QJsonObject command(const QJsonObject &request);
    const core::Settings &settings() const { return m_config.settings; }

private:
    struct Loaded;
    class Api;

    void loadPlugins();
    void unloadPlugin(Loaded &p);
    void handleHostSignal(const shade::Signal &s);
    void onWidget(const shade::Signal &s);
    void onPeek(const shade::Signal &s);
    void onDismissPeek(const shade::Signal &s);
    void onActivateWidget(const shade::Signal &s);
    void onTerminalRequest(const shade::Signal &s);
    void onTerminalClose(const shade::Signal &s);
    ShadeWindow *pointerWindow() const;

    void addScreen(QScreen *screen);
    void removeScreen(QScreen *screen);
    void requestWidgets(ShadeWindow *w);
    void recomputeActive();
    void onPeekRemoved(ShadeWindow *origin, const QString &plugin, const QString &id, const QString &reason);

    ShadeWindow *window(const QString &name) const;
    QList<ShadeWindow *> targets(const QString &name) const;
    Loaded *plugin(const QString &name);
    QJsonObject stateJson() const;

    core::Config m_config;
    core::SignalBus m_bus;
    core::Endpoint *m_hostEp = nullptr;
    core::Endpoint *m_ctlEp = nullptr;
    std::vector<std::unique_ptr<Loaded>> m_plugins;
    QList<QPointer<ShadeWindow>> m_windows;
    QSet<QString> m_active;
    std::unique_ptr<core::ControlServer> m_control;
    QPointer<ShadeWindow> m_lastWindow;
    bool m_shuttingDown = false;
};

} // namespace shade::app
