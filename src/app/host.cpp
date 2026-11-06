#include "host.h"

#include "base/paths.h"
#include "shadewindow.h"

#include <shade/pane.h>
#include <shade/theme.h>

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QPainter>
#include <QPluginLoader>
#include <QProcess>
#include <QScreen>

namespace shade::app
{

namespace
{

// Shown when no plugin delivers a widget.
class PlaceholderPane : public shade::PaneWidget
{
public:
    QSize sizeHint() const override { return QSize(260, 44); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(Theme::get().tx3());
        p.setFont(Theme::font(13));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("no plugins"));
    }
};

QJsonObject rect(const QRectF &r)
{
    return QJsonObject{{QStringLiteral("x"), r.x()}, {QStringLiteral("y"), r.y()}, {QStringLiteral("w"), r.width()}, {QStringLiteral("h"), r.height()}};
}

} // namespace

// What a plugin sees as its host.
class ShadeHost::Api : public shade::Host
{
public:
    Api(QString name, core::Endpoint *ep, QVariantMap config)
        : m_name(std::move(name)), m_ep(ep), m_config(std::move(config))
    {
    }
    QString pluginName() const override { return m_name; }
    shade::Bus *bus() override { return m_ep; }
    QVariantMap config() const override { return m_config; }

private:
    QString m_name;
    core::Endpoint *m_ep;
    QVariantMap m_config;
};

struct ShadeHost::Loaded
{
    base::PluginInfo info;
    int order = 0;
    std::unique_ptr<QPluginLoader> loader;
    shade::Plugin *instance = nullptr;
    std::unique_ptr<Api> api;
    core::Endpoint *ep = nullptr;
    bool active = false;
};

ShadeHost::ShadeHost(core::Config config, QObject *parent)
    : QObject(parent)
    , m_config(std::move(config))
{
    Theme::set(m_config.theme);
    m_hostEp = m_bus.endpoint(QString::fromLatin1(shade::kHost));
    m_ctlEp = m_bus.endpoint(QStringLiteral("shadectl"));
    for (const char *name : {shade::sig::Widget, shade::sig::Peek, shade::sig::DismissPeek, shade::sig::ActivateWidget,
                             shade::sig::TerminalRequest, shade::sig::TerminalClose}) {
        m_hostEp->handle(QString::fromLatin1(name), [this](const shade::Signal &s) { handleHostSignal(s); });
    }
    // Replies to control-socket signals are only logged.
    m_ctlEp->handle(QStringLiteral("*"), [](const shade::Signal &s) {
        qInfo().noquote() << "shadectl <-" << s.source << s.name << QVariant(s.payload).toJsonValue();
    });
}

ShadeHost::~ShadeHost() { shutdown(); }

bool ShadeHost::start(QString *error)
{
    for (const QString &w : m_config.warnings) qWarning().noquote() << "config:" << w;

    m_control = std::make_unique<core::ControlServer>([this](const QJsonObject &req) { return command(req); });
    QString why;
    if (!m_control->listen(base::socketPath(), &why)) {
        qWarning().noquote() << "shade: control socket disabled:" << why;
    }

    loadPlugins();

    const QList<QScreen *> screens = m_config.settings.monitors == QLatin1String("primary")
        ? QList<QScreen *>{QGuiApplication::primaryScreen()}
        : QGuiApplication::screens();
    for (QScreen *s : screens) {
        if (s) addScreen(s);
    }
    if (m_windows.isEmpty()) qWarning("shade: no monitors yet; waiting for one");
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *s) {
        if (m_config.settings.monitors == QLatin1String("all") || s == QGuiApplication::primaryScreen()) addScreen(s);
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, &ShadeHost::removeScreen);
    connect(qApp, &QGuiApplication::primaryScreenChanged, this, [this](QScreen *s) {
        if (m_config.settings.monitors == QLatin1String("primary")) {
            for (const auto &w : m_windows) {
                if (w && w->targetScreen() != s) removeScreen(w->targetScreen());
            }
            if (s) addScreen(s);
        }
    });
    Q_UNUSED(error);
    return true;
}

void ShadeHost::loadPlugins()
{
    const QVector<base::PluginInfo> found = base::scanPlugins(base::pluginSearchDirs());
    int order = 0;
    for (const base::PluginInfo &info : found) {
        if (!info.valid) {
            qWarning().noquote() << "shade: skipping" << info.file << "-" << info.error;
            continue;
        }
        if (m_config.settings.disabledPlugins.contains(info.name)) {
            qInfo().noquote() << "shade: plugin" << info.name << "disabled in config";
            continue;
        }
        if (plugin(info.name) || info.name == QLatin1String(shade::kHost) || info.name == QLatin1String("shadectl") ||
            info.name == QLatin1String("placeholder")) {
            qWarning().noquote() << "shade: skipping" << info.file << "- duplicate plugin name" << info.name;
            continue;
        }
        auto p = std::make_unique<Loaded>();
        p->info = info;
        p->order = order++;
        p->loader = std::make_unique<QPluginLoader>(info.file);
        p->loader->setLoadHints(QLibrary::ResolveAllSymbolsHint);
        QObject *root = p->loader->instance();
        p->instance = root ? qobject_cast<shade::Plugin *>(root) : nullptr;
        if (!p->instance) {
            qWarning().noquote() << "shade: cannot load" << info.file << "-" << p->loader->errorString();
            p->loader->unload();
            continue;
        }
        p->ep = m_bus.endpoint(info.name);
        // Shade-wide defaults a plugin section may override.
        QVariantMap section = m_config.pluginSection(info.name);
        if (!section.contains(QStringLiteral("terminal"))) section.insert(QStringLiteral("terminal"), m_config.settings.terminal);
        p->api = std::make_unique<Api>(info.name, p->ep, section);
        bool ok = false;
        try {
            ok = p->instance->load(p->api.get());
        } catch (...) {
            ok = false;
        }
        if (!ok) {
            qWarning().noquote() << "shade: plugin" << info.name << "failed to load";
            m_bus.removeEndpoint(info.name);
            p->loader->unload();
            continue;
        }
        qInfo().noquote() << "shade: loaded plugin" << info.name << info.version << "from" << info.file;
        m_plugins.push_back(std::move(p));
    }
}

void ShadeHost::unloadPlugin(Loaded &p)
{
    for (const auto &w : m_windows) {
        if (!w) continue;
        w->removeTerminals(p.info.name); // and their processes
        w->removePane(p.info.name);
    }
    m_bus.removeEndpoint(p.info.name);
    if (p.instance) {
        try {
            p.instance->unload();
        } catch (...) {
            qWarning().noquote() << "shade: plugin" << p.info.name << "threw while unloading";
        }
    }
    p.instance = nullptr;
    // Unloading the .so while Qt still caches metaobjects of its widgets is
    // unsafe; plugins are only unloaded at shutdown, so keep it mapped.
}

void ShadeHost::shutdown()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    if (m_control) m_control->close();
    // Withdraw peeks and widgets first, then plugins in reverse order.
    for (const auto &w : m_windows) delete w.data();
    m_windows.clear();
    for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it) unloadPlugin(**it);
    m_plugins.clear();
}

ShadeHost::Loaded *ShadeHost::plugin(const QString &name)
{
    for (auto &p : m_plugins) {
        if (p->info.name == name) return p.get();
    }
    return nullptr;
}

ShadeWindow *ShadeHost::window(const QString &name) const
{
    for (const auto &w : m_windows) {
        if (w && w->name() == name) return w;
    }
    return nullptr;
}

QList<ShadeWindow *> ShadeHost::targets(const QString &name) const
{
    QList<ShadeWindow *> out;
    for (const auto &w : m_windows) {
        if (w && (name.isEmpty() || w->name() == name)) out << w;
    }
    return out;
}

// --- monitors -------------------------------------------------------------

void ShadeHost::addScreen(QScreen *screen)
{
    if (m_shuttingDown) return;
    for (const auto &w : m_windows) {
        if (w && w->targetScreen() == screen) return;
    }
    auto *w = new ShadeWindow(screen, &m_config.settings);
    m_windows << w;
    w->setPlaceholder(new PlaceholderPane);
    connect(w, &ShadeWindow::activePluginChanged, this, &ShadeHost::recomputeActive);
    connect(w, &ShadeWindow::pointerEntered, this, [this, w]() { m_lastWindow = w; });
    connect(w, &ShadeWindow::terminalClosed, this, [this, w](const QString &owner, const QString &reason, int code) {
        if (m_shuttingDown || (!plugin(owner) && owner != QLatin1String("shadectl"))) return;
        QVariantMap payload{{QStringLiteral("reason"), reason}};
        if (code >= 0) payload.insert(QStringLiteral("exit_code"), code);
        m_hostEp->send(owner, QString::fromLatin1(shade::sig::TerminalClosed), payload, w->name());
    });
    connect(w, &ShadeWindow::peekRemoved, this, [this, w](const QString &plugin, const QString &id, const QString &reason) {
        onPeekRemoved(w, plugin, id, reason);
    });
    w->showOnLayer();
    requestWidgets(w);
    qInfo().noquote() << "shade: window on" << w->name();
}

void ShadeHost::removeScreen(QScreen *screen)
{
    for (int i = 0; i < m_windows.size(); ++i) {
        ShadeWindow *w = m_windows[i];
        if (w && w->targetScreen() == screen) {
            qInfo().noquote() << "shade: monitor" << w->name() << "removed";
            m_windows.removeAt(i);
            w->deleteLater();
            w->disconnect(this);
            recomputeActive();
            return;
        }
    }
}

void ShadeHost::requestWidgets(ShadeWindow *w)
{
    for (const auto &p : m_plugins) {
        m_hostEp->send(p->info.name, QString::fromLatin1(shade::sig::WidgetRequest),
                       QVariantMap{{QStringLiteral("window"), w->name()}}, w->name());
    }
}

void ShadeHost::recomputeActive()
{
    QSet<QString> now;
    for (const auto &w : m_windows) {
        if (!w) continue;
        const QString a = w->model()->activePlugin();
        if (!a.isEmpty() && plugin(a)) now.insert(a);
    }
    // Deactivations are processed before activations.
    for (const QString &name : m_active) {
        if (!now.contains(name)) m_hostEp->send(name, QString::fromLatin1(shade::sig::Deactivate));
    }
    for (const QString &name : now) {
        if (!m_active.contains(name)) m_hostEp->send(name, QString::fromLatin1(shade::sig::Activate));
    }
    m_active = now;
}

void ShadeHost::onPeekRemoved(ShadeWindow *origin, const QString &plugin, const QString &id, const QString &reason)
{
    // A peek the user dismissed (or that timed out) is gone everywhere.
    for (const auto &w : m_windows) {
        if (w && w != origin) {
            const auto &q = w->model()->queue().entries();
            const bool has = std::any_of(q.begin(), q.end(), [&](const core::PeekEntry &e) { return e.plugin == plugin && e.id == id; });
            if (has) {
                QSignalBlocker block(w);
                w->model()->dismissPeek(plugin, id, reason);
            }
        }
    }
    if (reason == QLatin1String("plugin") || !this->plugin(plugin)) return;
    m_hostEp->send(plugin, QString::fromLatin1(shade::sig::PeekDismissed),
                   QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("reason"), reason}});
}

// --- host signals -------------------------------------------------------------

void ShadeHost::handleHostSignal(const shade::Signal &s)
{
    if (m_shuttingDown || !plugin(s.source)) {
        if (s.source != QLatin1String("shadectl")) return;
    }
    if (s.name == QLatin1String(shade::sig::Widget)) onWidget(s);
    else if (s.name == QLatin1String(shade::sig::Peek)) onPeek(s);
    else if (s.name == QLatin1String(shade::sig::DismissPeek)) onDismissPeek(s);
    else if (s.name == QLatin1String(shade::sig::ActivateWidget)) onActivateWidget(s);
    else if (s.name == QLatin1String(shade::sig::TerminalRequest)) onTerminalRequest(s);
    else if (s.name == QLatin1String(shade::sig::TerminalClose)) onTerminalClose(s);
}

ShadeWindow *ShadeHost::pointerWindow() const
{
    if (m_lastWindow) return m_lastWindow;
    for (const auto &w : m_windows) {
        if (w && w->targetScreen() == QGuiApplication::primaryScreen()) return w;
    }
    for (const auto &w : m_windows) {
        if (w) return w;
    }
    return nullptr;
}

void ShadeHost::onTerminalRequest(const shade::Signal &s)
{
    auto reject = [&](const QString &why, const QString &reason) {
        // Diagnostics name the plugin and the problem, not the command line.
        qWarning().noquote() << "shade: rejected terminal_request from" << s.source << "-" << why;
        m_hostEp->send(s.source, QString::fromLatin1(shade::sig::TerminalClosed),
                       QVariantMap{{QStringLiteral("reason"), reason}, {QStringLiteral("error"), why}}, s.window);
    };
    const QString command = s.payload.value(QStringLiteral("command")).toString().trimmed();
    if (command.isEmpty()) return reject(QStringLiteral("missing command"), QStringLiteral("rejected"));
    QStringList argv;
    if (!shade::splitCommand(command, &argv) || argv.isEmpty()) return reject(QStringLiteral("unparseable command"), QStringLiteral("rejected"));
    bool okW = false, okH = false;
    const int width = s.payload.value(QStringLiteral("width")).toInt(&okW);
    const int height = s.payload.value(QStringLiteral("height")).toInt(&okH);
    if (!okW || !okH || width < 80 || height < 80 || width > 8192 || height > 8192) {
        return reject(QStringLiteral("size must be two integers in [80, 8192]"), QStringLiteral("rejected"));
    }
    ShadeWindow *w = s.window.isEmpty() ? pointerWindow() : window(s.window);
    if (!w) return reject(QStringLiteral("no window '%1'").arg(s.window), QStringLiteral("rejected"));
    QString error;
    if (!w->openTerminal(s.source, argv, QSize(width, height), &error)) {
        qWarning().noquote() << "shade: terminal for" << s.source << "failed:" << error;
        m_hostEp->send(s.source, QString::fromLatin1(shade::sig::TerminalClosed),
                       QVariantMap{{QStringLiteral("reason"), QStringLiteral("failed")}, {QStringLiteral("error"), error}}, w->name());
    }
}

void ShadeHost::onTerminalClose(const shade::Signal &s)
{
    // A plugin can only close its own terminal; closing none is a no-op.
    for (ShadeWindow *w : targets(s.window)) w->closeTerminal(s.source, QStringLiteral("closed"));
}

void ShadeHost::onWidget(const shade::Signal &s)
{
    Loaded *p = plugin(s.source);
    const QString winName = s.window.isEmpty() ? s.payload.value(QStringLiteral("window")).toString() : s.window;
    ShadeWindow *w = window(winName);
    auto *widget = qobject_cast<shade::PaneWidget *>(s.payload.value(QStringLiteral("widget")).value<QObject *>());
    if (!p || !w || !widget) {
        qWarning().noquote() << "shade: rejected widget from" << s.source << "for window" << winName
                             << (widget ? "" : "(not a shade::PaneWidget)");
        delete widget;
        return;
    }
    if (w->pane(s.source)) {
        qWarning().noquote() << "shade: rejected second widget from" << s.source << "for window" << winName;
        widget->deleteLater();
        return;
    }
    widget->setObjectName(s.source);
    widget->setProperty("shadePlugin", s.source);
    w->addPane(s.source, p->order, widget);
}

void ShadeHost::onPeek(const shade::Signal &s)
{
    const QVariantMap &pl = s.payload;
    core::PeekEntry e;
    e.plugin = s.source;
    e.id = pl.value(QStringLiteral("id"), QStringLiteral("default")).toString();
    if (e.id.isEmpty()) e.id = QStringLiteral("default");
    bool ok = true;
    e.priority = pl.contains(QStringLiteral("priority")) ? pl.value(QStringLiteral("priority")).toInt(&ok) : 2;
    if (!ok || e.priority < 1 || e.priority > 3) {
        qWarning().noquote() << "shade: rejected peek" << e.id << "from" << s.source << "- invalid priority";
        return;
    }
    const QString expand = pl.value(QStringLiteral("expand"), QStringLiteral("hover")).toString();
    if (expand != QLatin1String("hover") && expand != QLatin1String("top") && expand != QLatin1String("none")) {
        qWarning().noquote() << "shade: rejected peek" << e.id << "from" << s.source << "- invalid expand" << expand;
        return;
    }
    e.expand = core::parseExpand(expand);
    const int timeout = pl.value(QStringLiteral("timeout"), -1).toInt();
    e.timeoutMs = timeout < 0 ? m_config.settings.peekTimeoutMs : timeout;
    e.data = pl.value(QStringLiteral("data")).toMap();
    int delivered = 0;
    for (ShadeWindow *w : targets(s.window)) {
        const QSizeF size = w->peekSize(s.source, {e.id, e.data});
        if (size.isEmpty()) continue; // no widget on this window
        core::PeekEntry copy = e;
        copy.size = size;
        w->model()->publishPeek(copy);
        ++delivered;
    }
    if (!delivered) qWarning().noquote() << "shade: rejected peek" << e.id << "from" << s.source << "- plugin has no widget";
}

void ShadeHost::onDismissPeek(const shade::Signal &s)
{
    const QString id = s.payload.value(QStringLiteral("id")).toString();
    for (ShadeWindow *w : targets(s.window)) {
        QSignalBlocker block(w); // plugin asked for it: no peek_dismissed echo
        w->model()->dismissPeek(s.source, id, QStringLiteral("plugin"));
    }
}

void ShadeHost::onActivateWidget(const shade::Signal &s)
{
    const QString name = s.payload.value(QStringLiteral("plugin"), s.source).toString();
    QList<ShadeWindow *> ws = targets(s.window);
    if (s.window.isEmpty()) {
        ShadeWindow *preferred = m_lastWindow ? m_lastWindow.data() : nullptr;
        if (!preferred) {
            for (ShadeWindow *w : ws) {
                if (w->targetScreen() == QGuiApplication::primaryScreen()) preferred = w;
            }
        }
        if (!preferred && !ws.isEmpty()) preferred = ws.first();
        ws = preferred ? QList<ShadeWindow *>{preferred} : QList<ShadeWindow *>{};
    }
    bool ok = false;
    for (ShadeWindow *w : ws) ok |= w->model()->focusPlugin(name);
    if (!ok) qWarning().noquote() << "shade: rejected activate_widget from" << s.source << "- no widget named" << name;
}

// --- control commands ---------------------------------------------------------

QJsonObject ShadeHost::stateJson() const
{
    QJsonArray windows;
    for (const auto &w : m_windows) {
        if (!w) continue;
        const core::ShadeModel *m = w->model();
        QJsonArray panes, peeks;
        for (const core::PaneInfo &p : m->panes()) panes << QJsonObject{{QStringLiteral("plugin"), p.plugin}, {QStringLiteral("w"), p.size.width()}, {QStringLiteral("h"), p.size.height()}};
        for (const core::PeekEntry &e : m->queue().entries()) {
            peeks << QJsonObject{{QStringLiteral("plugin"), e.plugin}, {QStringLiteral("id"), e.id},
                                 {QStringLiteral("priority"), e.priority}, {QStringLiteral("expand"), core::expandName(e.expand)},
                                 {QStringLiteral("data"), QJsonObject::fromVariantMap(e.data)}};
        }
        windows << QJsonObject{
            {QStringLiteral("name"), w->name()},
            {QStringLiteral("state"), m->stateName()},
            {QStringLiteral("selected"), m->selectedPlugin()},
            {QStringLiteral("active"), m->activePlugin()},
            {QStringLiteral("live"), w->isLive()},
            {QStringLiteral("settled"), m->geometrySettled()},
            {QStringLiteral("morph"), m->morph()},
            {QStringLiteral("pill"), rect(m->pillRect())},
            {QStringLiteral("terminals"), w->terminalsJson(m_config.settings.debugCommands || qEnvironmentVariableIsSet("SHADE_DEBUG"))},
            {QStringLiteral("canvas"), QJsonObject{{QStringLiteral("w"), w->width()}, {QStringLiteral("h"), w->height()}}},
            {QStringLiteral("input"), rect(w->inputRegion().boundingRect())},
            {QStringLiteral("panes"), panes},
            {QStringLiteral("peeks"), peeks},
        };
    }
    QJsonArray plugins;
    for (const auto &p : m_plugins) {
        plugins << QJsonObject{{QStringLiteral("name"), p->info.name}, {QStringLiteral("version"), p->info.version},
                               {QStringLiteral("file"), p->info.file}, {QStringLiteral("active"), m_active.contains(p->info.name)}};
    }
    return QJsonObject{{QStringLiteral("windows"), windows}, {QStringLiteral("plugins"), plugins}};
}

QJsonObject ShadeHost::command(const QJsonObject &req)
{
    const QString cmd = req.value(QStringLiteral("cmd")).toString();
    const QString win = req.value(QStringLiteral("window")).toString();
    const bool debug = m_config.settings.debugCommands || qEnvironmentVariableIsSet("SHADE_DEBUG");
    auto ok = [](QJsonObject o = {}) {
        o.insert(QStringLiteral("ok"), true);
        return o;
    };
    if (!win.isEmpty() && !window(win)) return core::errorReply(QStringLiteral("no window named '%1'").arg(win));

    if (cmd == QLatin1String("ping")) return ok({{QStringLiteral("pong"), true}, {QStringLiteral("pid"), qint64(QCoreApplication::applicationPid())}});
    if (cmd == QLatin1String("version")) {
        return ok({{QStringLiteral("version"), QCoreApplication::applicationVersion()},
                   {QStringLiteral("api"), QStringLiteral("%1.%2").arg(SHADE_API_MAJOR).arg(SHADE_API_MINOR)}});
    }
    if (cmd == QLatin1String("state")) return ok(stateJson());
    if (cmd == QLatin1String("plugins")) return ok({{QStringLiteral("plugins"), stateJson().value(QStringLiteral("plugins"))}});
    if (cmd == QLatin1String("expand")) {
        for (ShadeWindow *w : targets(win)) w->model()->expand();
        return ok();
    }
    if (cmd == QLatin1String("collapse")) {
        for (ShadeWindow *w : targets(win)) w->model()->collapse();
        return ok();
    }
    if (cmd == QLatin1String("focus")) {
        const QString p = req.value(QStringLiteral("plugin")).toString();
        bool any = false;
        for (ShadeWindow *w : targets(win)) any |= w->model()->focusPlugin(p);
        return any ? ok() : core::errorReply(QStringLiteral("no widget named '%1'").arg(p));
    }
    if (cmd == QLatin1String("dismiss")) {
        const QString p = req.value(QStringLiteral("plugin")).toString();
        const QString id = req.value(QStringLiteral("id")).toString();
        for (ShadeWindow *w : targets(win)) {
            if (p.isEmpty()) {
                while (const core::PeekEntry *f = w->model()->queue().front()) w->model()->dismissPeek(f->plugin, f->id, QStringLiteral("user"));
            } else {
                w->model()->dismissPeek(p, id, QStringLiteral("user"));
            }
        }
        return ok();
    }
    if (cmd == QLatin1String("signal")) {
        shade::Signal s;
        s.name = req.value(QStringLiteral("name")).toString();
        s.destination = req.value(QStringLiteral("destination")).toString();
        s.window = win;
        s.payload = req.value(QStringLiteral("payload")).toObject().toVariantMap();
        if (!m_ctlEp->send(s)) return core::errorReply(QStringLiteral("signal rejected: %1").arg(m_bus.diagnostics().value(m_bus.diagnostics().size() - 1)));
        return ok();
    }
    if (cmd == QLatin1String("quit")) {
        QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
        return ok();
    }

    // Debug/test commands: synthetic input and frame capture.
    static const QStringList debugCmds = {QStringLiteral("pointer"), QStringLiteral("leave"), QStringLiteral("click"),
                                          QStringLiteral("wheel"), QStringLiteral("render"), QStringLiteral("type")};
    if (debugCmds.contains(cmd)) {
        if (!debug) return core::errorReply(QStringLiteral("'%1' needs debug_commands = true").arg(cmd));
        QList<ShadeWindow *> ws = targets(win);
        if (ws.isEmpty()) return core::errorReply(QStringLiteral("no windows"));
        ShadeWindow *w = ws.first();
        m_lastWindow = w;
        const QPointF pos(req.value(QStringLiteral("x")).toDouble(), req.value(QStringLiteral("y")).toDouble());
        if (cmd == QLatin1String("pointer")) w->injectPointer(pos);
        else if (cmd == QLatin1String("leave")) w->injectLeave();
        else if (cmd == QLatin1String("click")) w->injectClick(pos);
        else if (cmd == QLatin1String("wheel")) {
            w->injectWheel(req.value(QStringLiteral("dx")).toDouble(), req.value(QStringLiteral("dy")).toDouble(),
                           req.value(QStringLiteral("end")).toBool(), req.value(QStringLiteral("touchpad")).toBool());
        } else if (cmd == QLatin1String("type")) {
            if (!w->typeIntoTerminal(req.value(QStringLiteral("owner")).toString(), req.value(QStringLiteral("text")).toString())) {
                return core::errorReply(QStringLiteral("no terminal to type into"));
            }
        } else if (cmd == QLatin1String("render")) {
            const QString path = req.value(QStringLiteral("path")).toString();
            if (path.isEmpty()) return core::errorReply(QStringLiteral("render needs \"path\""));
            if (!w->renderFrame().save(path)) return core::errorReply(QStringLiteral("cannot write %1").arg(path));
            return ok({{QStringLiteral("path"), path}});
        }
        return ok();
    }
    return core::errorReply(QStringLiteral("unknown command '%1'").arg(cmd));
}

} // namespace shade::app
