#pragma once

#include "sources.h"

#include <shade/sdk.h>

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

class QTimer;

namespace stats
{

class StatsPane;

// The stats plugin: one set of data sources (shared by every monitor's
// widget) and one StatsPane per window.
//
// Lifecycle (mixed): the clock and Wi-Fi signal polling run only while the
// pane is active; battery, network, Bluetooth and volume are event driven
// (D-Bus signals, `pactl subscribe`) and always on, because their changes
// are announced as peeks while the shade is collapsed.
class StatsPlugin : public QObject, public shade::Plugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ShadePlugin_iid FILE "meta.json")
    Q_INTERFACES(shade::Plugin)

public:
    bool load(shade::Host *host) override;
    void unload() override;

    Battery *battery() const { return m_battery; }
    Network *network() const { return m_network; }
    Bluetooth *bluetooth() const { return m_bluetooth; }
    Volume *volume() const { return m_volume; }

    QStringList order() const { return m_order; }
    QString timeFormat() const { return m_timeFormat; }
    bool hideMissingBattery() const { return m_hideMissingBattery; }
    // Open the tool for "network", "bluetooth" or "volume": in a terminal
    // embedded below the shade on `window` (default), or in an external
    // terminal emulator (terminal_mode = "external").
    void launch(const QString &what, const QString &window = {});

signals:
    void stateChanged();
    void timeTick();

private:
    void onEvent(const stats::Event &e);
    void setActive(bool active);
    void launchExternal(const QString &command);

    shade::Host *m_host = nullptr;
    QVariantMap m_cfg;
    QStringList m_order;
    QString m_timeFormat;
    bool m_hideMissingBattery = false;
    QVariantMap m_peeks;
    QTimer *m_clock = nullptr;
    Battery *m_battery = nullptr;
    Network *m_network = nullptr;
    Bluetooth *m_bluetooth = nullptr;
    Volume *m_volume = nullptr;
    QList<QPointer<StatsPane>> m_panes;
    QList<int> m_handlers;
    QString m_lastEmbedded;
};

} // namespace stats
