#include "plugin.h"

#include "pane.h"

#include <shade/pane.h>

#include <QDateTime>
#include <QDebug>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace stats
{

bool StatsPlugin::load(shade::Host *host)
{
    registerDBusTypes();
    m_host = host;
    m_cfg = host->config();

    m_order = m_cfg.value(QStringLiteral("show"), QStringList{QStringLiteral("time"), QStringLiteral("network"), QStringLiteral("bluetooth"),
                                                               QStringLiteral("battery"), QStringLiteral("volume")}).toStringList();
    m_timeFormat = m_cfg.value(QStringLiteral("time_format"), QStringLiteral("HH:mm:ss")).toString();
    m_hideMissingBattery = m_cfg.value(QStringLiteral("hide_missing_battery"), true).toBool();
    m_peeks = m_cfg.value(QStringLiteral("peeks")).toMap();

    QList<int> low;
    for (const QVariant &v : m_cfg.value(QStringLiteral("battery_low"), QVariantList{20, 10, 5}).toList()) low << v.toInt();

    const QDBusConnection sys = systemBus(m_cfg.value(QStringLiteral("system_bus"), QStringLiteral("system")).toString());
    m_battery = new Battery(sys, low, this);
    m_network = new Network(sys, m_cfg.value(QStringLiteral("network_backend"), QStringLiteral("auto")).toString(), this);
    m_bluetooth = new Bluetooth(sys, this);
    m_volume = new Volume(m_cfg.value(QStringLiteral("pactl"), QStringLiteral("pactl")).toString(),
                          m_cfg.value(QStringLiteral("volume_step"), 5).toInt(), this);
    for (QObject *src : std::initializer_list<QObject *>{m_battery, m_network, m_bluetooth, m_volume}) {
        connect(src, SIGNAL(changed()), this, SIGNAL(stateChanged()));
    }
    connect(m_battery, &Battery::notable, this, &StatsPlugin::onEvent);
    connect(m_network, &Network::notable, this, &StatsPlugin::onEvent);
    connect(m_bluetooth, &Bluetooth::notable, this, &StatsPlugin::onEvent);
    connect(m_volume, &Volume::notable, this, &StatsPlugin::onEvent);

    m_clock = new QTimer(this);
    m_clock->setTimerType(Qt::PreciseTimer);
    m_clock->setSingleShot(true);
    connect(m_clock, &QTimer::timeout, this, [this]() {
        emit timeTick();
        // Re-align to the next second boundary every tick (follows clock
        // changes, NTP, timezone).
        m_clock->start(1000 - int(QDateTime::currentMSecsSinceEpoch() % 1000) + 5);
    });

    shade::Bus *bus = host->bus();
    m_handlers << bus->handle(QString::fromLatin1(shade::sig::WidgetRequest), [this](const shade::Signal &s) {
        auto *pane = new StatsPane(this, s.window);
        m_panes << pane;
        m_host->bus()->send(QString::fromLatin1(shade::kHost), QString::fromLatin1(shade::sig::Widget),
                            QVariantMap{{QStringLiteral("widget"), QVariant::fromValue<QObject *>(pane)}}, s.window);
    });
    m_handlers << bus->handle(QString::fromLatin1(shade::sig::Activate), [this](const shade::Signal &) { setActive(true); });
    m_handlers << bus->handle(QString::fromLatin1(shade::sig::Deactivate), [this](const shade::Signal &) { setActive(false); });
    // `shadectl signal stats launch '{"what":"network"}'` (e.g. from a keybinding).
    m_handlers << bus->handle(QStringLiteral("launch"), [this](const shade::Signal &s) {
        launch(s.payload.value(QStringLiteral("what")).toString(), s.window);
    });
    // A shade built without terminal support: fall back to the external one.
    m_handlers << bus->handle(QString::fromLatin1(shade::sig::TerminalClosed), [this](const shade::Signal &s) {
        if (s.payload.value(QStringLiteral("reason")).toString() == QLatin1String("failed") &&
            s.payload.value(QStringLiteral("error")).toString().contains(QLatin1String("without terminal support")) && !m_lastEmbedded.isEmpty()) {
            launchExternal(m_lastEmbedded);
        }
    });
    // Debug/testing helper: `shadectl signal stats test_peek '{"kind":"battery"}'`.
    m_handlers << bus->handle(QStringLiteral("test_peek"), [this](const shade::Signal &s) {
        const QString kind = s.payload.value(QStringLiteral("kind"), QStringLiteral("network")).toString();
        Event e{kind, QString(), s.payload.value(QStringLiteral("text")).toString(), s.payload.value(QStringLiteral("priority"), 2).toInt(),
                s.payload.value(QStringLiteral("value"), -1).toInt()};
        if (kind == QLatin1String("battery")) e.icon = m_battery->icon(), e.text = e.text.isEmpty() ? m_battery->describe() : e.text;
        else if (kind == QLatin1String("bluetooth")) e.icon = m_bluetooth->icon(), e.text = e.text.isEmpty() ? m_bluetooth->describe() : e.text;
        else if (kind == QLatin1String("volume")) e.icon = m_volume->icon(), e.text = e.text.isEmpty() ? QStringLiteral("Volume") : e.text, e.value = e.value < 0 ? m_volume->level() : e.value;
        else e.icon = m_network->icon(), e.text = e.text.isEmpty() ? m_network->describe() : e.text;
        onEvent(e);
    });
    return true;
}

void StatsPlugin::unload()
{
    for (int id : m_handlers) m_host->bus()->unhandle(id);
    m_handlers.clear();
    if (m_clock) m_clock->stop();
    // Panes are owned (and deleted) by the host windows.
    delete m_volume;
    m_volume = nullptr;
}

void StatsPlugin::setActive(bool active)
{
    if (active) {
        emit timeTick();
        m_clock->start(1000 - int(QDateTime::currentMSecsSinceEpoch() % 1000) + 5);
    } else {
        m_clock->stop();
    }
    m_network->setActive(active);
}

void StatsPlugin::onEvent(const stats::Event &e)
{
    if (!m_peeks.value(e.kind, true).toBool()) return;
    QVariantMap data{{QStringLiteral("icon"), e.icon}, {QStringLiteral("text"), e.text}};
    if (e.value >= 0) data.insert(QStringLiteral("value"), e.value);
    const int timeout = e.priority >= 3 ? 8000 : e.kind == QLatin1String("volume") ? 1800 : -1;
    m_host->bus()->send(QString::fromLatin1(shade::kHost), QString::fromLatin1(shade::sig::Peek),
                        shade::peekPayload(e.kind, data, shade::PeekPriority(std::clamp(e.priority, 1, 3)), QString::fromLatin1(shade::kExpandHover), timeout));
}

void StatsPlugin::launch(const QString &what, const QString &window)
{
    static const QHash<QString, QString> defaults{
        {QStringLiteral("network"), QStringLiteral("iwctl")},
        {QStringLiteral("bluetooth"), QStringLiteral("bluetuith")},
        {QStringLiteral("volume"), QStringLiteral("pulsemixer")},
    };
    const QString command = m_cfg.value(what + QStringLiteral("_command"), defaults.value(what)).toString();
    if (command.isEmpty()) return;
    if (m_cfg.value(QStringLiteral("terminal_mode"), QStringLiteral("embedded")).toString() == QLatin1String("external")) {
        launchExternal(command);
        return;
    }
    // Embedded: the shade hosts the terminal below the pill. A size per tool
    // (`network_size = [w, h]`) overrides terminal_width/terminal_height.
    int width = m_cfg.value(QStringLiteral("terminal_width"), 720).toInt();
    int height = m_cfg.value(QStringLiteral("terminal_height"), 420).toInt();
    const QVariantList size = m_cfg.value(what + QStringLiteral("_size")).toList();
    if (size.size() == 2) {
        width = size[0].toInt();
        height = size[1].toInt();
    }
    m_lastEmbedded = command;
    m_host->bus()->send(QString::fromLatin1(shade::kHost), QString::fromLatin1(shade::sig::TerminalRequest),
                        QVariantMap{{QStringLiteral("command"), command}, {QStringLiteral("width"), width}, {QStringLiteral("height"), height}},
                        window);
}

void StatsPlugin::launchExternal(const QString &command)
{
    QString terminal = m_cfg.value(QStringLiteral("terminal")).toString();
    if (terminal.isEmpty()) terminal = qEnvironmentVariable("TERMINAL");
    if (terminal.isEmpty()) terminal = QStringLiteral("kitty");
    QStringList args = m_cfg.contains(QStringLiteral("terminal_args"))
        ? m_cfg.value(QStringLiteral("terminal_args")).toStringList()
        : (terminal.endsWith(QLatin1String("kitty")) || terminal.endsWith(QLatin1String("foot")) ? QStringList{} : QStringList{QStringLiteral("-e")});
    QStringList argv;
    if (!shade::splitCommand(command, &argv) || argv.isEmpty()) {
        qWarning().noquote() << "stats: cannot parse command for" << command.section(u' ', 0, 0);
        return;
    }
    args << argv;
    if (!QProcess::startDetached(terminal, args)) qWarning().noquote() << "stats: cannot start" << terminal << args.join(u' ');
}

} // namespace stats
