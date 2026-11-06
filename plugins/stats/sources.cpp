#include "sources.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

#include <cmath>

namespace stats
{

namespace
{
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kObjMgr = QStringLiteral("org.freedesktop.DBus.ObjectManager");

QString icon(const QString &name) { return QStringLiteral(":/shade/stats/icons/%1.svg").arg(name); }

// Async Properties.GetAll; `done` receives an empty map on error.
void getAll(QDBusConnection bus, QObject *ctx, const QString &service, const QString &path, const QString &iface,
            std::function<void(const QVariantMap &, bool ok)> done)
{
    QDBusMessage m = QDBusMessage::createMethodCall(service, path, kProps, QStringLiteral("GetAll"));
    m << iface;
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(m, 3000), ctx);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, ctx, [w, done]() {
        QDBusPendingReply<QVariantMap> r = *w;
        w->deleteLater();
        done(r.isError() ? QVariantMap() : r.value(), !r.isError());
    });
}

void managedObjects(QDBusConnection bus, QObject *ctx, const QString &service, std::function<void(const ManagedObjects &, bool)> done)
{
    QDBusMessage m = QDBusMessage::createMethodCall(service, QStringLiteral("/"), kObjMgr, QStringLiteral("GetManagedObjects"));
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(m, 5000), ctx);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, ctx, [w, done]() {
        QDBusPendingReply<ManagedObjects> r = *w;
        w->deleteLater();
        done(r.isError() ? ManagedObjects() : r.value(), !r.isError());
    });
}

void nameHasOwner(QDBusConnection bus, QObject *ctx, const QString &name, std::function<void(bool)> done)
{
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                                    QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
    m << name;
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(m, 2000), ctx);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, ctx, [w, done]() {
        QDBusPendingReply<bool> r = *w;
        w->deleteLater();
        done(!r.isError() && r.value());
    });
}

// Unwraps QDBusVariant/QDBusArgument nesting a QVariant may carry.
QVariant plain(const QVariant &v)
{
    if (v.canConvert<QDBusVariant>() && v.userType() == qMetaTypeId<QDBusVariant>()) return v.value<QDBusVariant>().variant();
    return v;
}
} // namespace

void registerDBusTypes()
{
    static bool done = false;
    if (done) return;
    done = true;
    qRegisterMetaType<Event>("stats::Event");
    qDBusRegisterMetaType<InterfaceMap>();
    qDBusRegisterMetaType<ManagedObjects>();
}

QDBusConnection systemBus(const QString &configured)
{
    const QString env = qEnvironmentVariable("SHADE_STATS_BUS");
    const QString which = env.isEmpty() ? configured : env;
    return which == QLatin1String("session") ? QDBusConnection::sessionBus() : QDBusConnection::systemBus();
}

// ===========================================================================
// Battery (UPower DisplayDevice)
// ===========================================================================

namespace
{
const QString kUPower = QStringLiteral("org.freedesktop.UPower");
const QString kDisplayDevice = QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice");
const QString kUPowerDevice = QStringLiteral("org.freedesktop.UPower.Device");
} // namespace

Battery::Battery(QDBusConnection bus, QList<int> low, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_low(std::move(low))
{
    std::sort(m_low.begin(), m_low.end(), std::greater<int>());
    m_watcher = new QDBusServiceWatcher(kUPower, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this, &Battery::refresh);
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this]() {
        m_present = false;
        emit changed();
    });
    m_bus.connect(kUPower, kDisplayDevice, kProps, QStringLiteral("PropertiesChanged"), this,
                  SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    refresh();
}

void Battery::refresh()
{
    getAll(m_bus, this, kUPower, kDisplayDevice, kUPowerDevice, [this](const QVariantMap &props, bool ok) {
        if (!ok) {
            m_present = false;
            m_loaded = true;
            emit changed();
            return;
        }
        apply(props, !m_loaded);
        m_loaded = true;
    });
}

void Battery::onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated)
{
    if (iface != kUPowerDevice) return;
    if (!m_loaded) return; // the pending GetAll will deliver the full state
    if (!invalidated.isEmpty()) {
        refresh();
        return;
    }
    apply(changed, false);
}

void Battery::apply(const QVariantMap &props, bool initial)
{
    const State before = m_state;
    if (props.contains(QStringLiteral("IsPresent"))) m_present = plain(props.value(QStringLiteral("IsPresent"))).toBool();
    if (props.contains(QStringLiteral("Type")) && plain(props.value(QStringLiteral("Type"))).toUInt() != 2) m_present = false;
    if (props.contains(QStringLiteral("Percentage"))) m_percentage = plain(props.value(QStringLiteral("Percentage"))).toDouble();
    if (props.contains(QStringLiteral("State"))) {
        const uint s = plain(props.value(QStringLiteral("State"))).toUInt();
        m_state = s <= 6 ? State(s) : State::Unknown;
    }
    emit changed();
    if (initial || !m_present) {
        m_lastLowWarned = 101;
        return;
    }
    const int pct = int(std::lround(m_percentage));
    if (m_state != before) {
        Event e{QStringLiteral("battery"), this->icon(), describe(), 2, pct};
        if (m_state == State::Charging || m_state == State::PendingCharge || m_state == State::Discharging ||
            m_state == State::Full) {
            emit notable(e);
        }
        if (charging()) m_lastLowWarned = 101;
    }
    if (m_state == State::Discharging) {
        for (int t : m_low) {
            if (pct <= t && t < m_lastLowWarned) {
                m_lastLowWarned = t;
                emit notable(Event{QStringLiteral("battery"), this->icon(), QStringLiteral("Battery low · %1%").arg(pct), 3, pct});
                break;
            }
        }
    }
}

QString Battery::icon() const
{
    if (!m_present) return stats::icon(QStringLiteral("battery-missing"));
    const int level = std::clamp(int(std::lround(m_percentage / 10.0)) * 10, 0, 100);
    return stats::icon(QStringLiteral("battery-%1%2").arg(level).arg(charging() && m_state != State::Full ? QStringLiteral("-charging") : QString()));
}

QString Battery::describe() const
{
    const int pct = int(std::lround(m_percentage));
    switch (m_state) {
    case State::Charging:
    case State::PendingCharge: return QStringLiteral("Charging · %1%").arg(pct);
    case State::Discharging:
    case State::PendingDischarge: return QStringLiteral("On battery · %1%").arg(pct);
    case State::Full: return QStringLiteral("Fully charged");
    case State::Empty: return QStringLiteral("Battery empty");
    case State::Unknown: break;
    }
    return QStringLiteral("Battery %1%").arg(pct);
}

// ===========================================================================
// Network (iwd / NetworkManager / sysfs wired)
// ===========================================================================

namespace
{
const QString kIwd = QStringLiteral("net.connman.iwd");
const QString kIwdStation = QStringLiteral("net.connman.iwd.Station");
const QString kIwdNetwork = QStringLiteral("net.connman.iwd.Network");
const QString kNM = QStringLiteral("org.freedesktop.NetworkManager");
const QString kNMPath = QStringLiteral("/org/freedesktop/NetworkManager");

int dbmToPercent(int dbm) { return std::clamp(2 * (dbm + 100), 0, 100); }
} // namespace

Network::Network(QDBusConnection bus, const QString &backend, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_requested(backend.isEmpty() ? QStringLiteral("auto") : backend)
{
    m_wiredTimer = new QTimer(this);
    m_wiredTimer->setInterval(3000);
    connect(m_wiredTimer, &QTimer::timeout, this, &Network::pollWired);
    m_wiredTimer->start();
    m_strengthTimer = new QTimer(this);
    m_strengthTimer->setInterval(10000);
    connect(m_strengthTimer, &QTimer::timeout, this, [this]() {
        if (m_backend == QLatin1String("iwd")) iwdPollStrength();
        else if (m_backend == QLatin1String("networkmanager")) nmRefresh();
    });
    pollWired();
    chooseBackend();
}

void Network::setActive(bool active)
{
    m_active = active;
    // Signal strength is the only polled value; poll it while visible.
    if (active) {
        m_strengthTimer->start();
        if (m_backend == QLatin1String("iwd")) iwdPollStrength();
        else if (m_backend == QLatin1String("networkmanager")) nmRefresh();
        pollWired();
    } else {
        m_strengthTimer->stop();
    }
}

void Network::chooseBackend()
{
    auto useIwd = [this]() {
        m_backend = QStringLiteral("iwd");
        if (!m_watcher) {
            m_watcher = new QDBusServiceWatcher(kIwd, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
            connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this, &Network::iwdScan);
            connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this]() {
                m_stations.clear();
                m_networks.clear();
                iwdRecompute();
            });
        }
        m_bus.connect(kIwd, QStringLiteral("/"), kObjMgr, QStringLiteral("InterfacesAdded"), this,
                      SLOT(onIwdInterfacesAdded(QDBusObjectPath, stats::InterfaceMap)));
        m_bus.connect(kIwd, QStringLiteral("/"), kObjMgr, QStringLiteral("InterfacesRemoved"), this,
                      SLOT(onIwdInterfacesRemoved(QDBusObjectPath, QStringList)));
        m_bus.connect(kIwd, QString(), kProps, QStringLiteral("PropertiesChanged"), this,
                      SLOT(onPropertiesChanged(QString, QVariantMap, QStringList, QDBusMessage)));
        iwdScan();
    };
    auto useNm = [this]() {
        m_backend = QStringLiteral("networkmanager");
        if (!m_watcher) {
            m_watcher = new QDBusServiceWatcher(kNM, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
            connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this, &Network::nmRefresh);
            connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this]() {
                m_nm.clear();
                m_nmSsid.clear();
                m_wifiConnected = false;
                recompute();
            });
        }
        m_bus.connect(kNM, kNMPath, kProps, QStringLiteral("PropertiesChanged"), this,
                      SLOT(onPropertiesChanged(QString, QVariantMap, QStringList, QDBusMessage)));
        nmRefresh();
    };

    if (m_requested == QLatin1String("iwd")) {
        useIwd();
    } else if (m_requested == QLatin1String("networkmanager") || m_requested == QLatin1String("nm")) {
        useNm();
    } else if (m_requested == QLatin1String("none")) {
        m_backend = QStringLiteral("none");
        m_loaded = true;
        recompute(false);
    } else {
        // auto: prefer iwd, then NetworkManager; wired-only otherwise.
        nameHasOwner(m_bus, this, kIwd, [this, useIwd, useNm](bool iwd) {
            if (iwd) {
                useIwd();
                return;
            }
            nameHasOwner(m_bus, this, kNM, [this, useIwd, useNm](bool nm) {
                if (nm) {
                    useNm();
                    return;
                }
                m_backend = QStringLiteral("none");
                m_loaded = true;
                recompute(false);
                // Pick up whichever wireless daemon starts later.
                auto *late = new QDBusServiceWatcher(kIwd, m_bus, QDBusServiceWatcher::WatchForRegistration, this);
                late->addWatchedService(kNM);
                connect(late, &QDBusServiceWatcher::serviceRegistered, this, [this, late, useIwd, useNm](const QString &s) {
                    if (m_backend != QLatin1String("none")) return;
                    late->deleteLater();
                    if (s == kIwd) useIwd();
                    else useNm();
                });
            });
        });
    }
}

void Network::iwdScan()
{
    managedObjects(m_bus, this, kIwd, [this](const ManagedObjects &objs, bool ok) {
        m_stations.clear();
        m_networks.clear();
        if (ok) {
            for (auto it = objs.cbegin(); it != objs.cend(); ++it) {
                const QString path = it.key().path();
                if (it->contains(kIwdStation)) m_stations.insert(path, it->value(kIwdStation));
                if (it->contains(kIwdNetwork)) m_networks.insert(path, it->value(kIwdNetwork));
            }
        }
        const bool first = !m_loaded;
        m_loaded = true;
        m_quiet = first; // the initial state is not an event
        iwdRecompute();
        m_quiet = false;
        iwdPollStrength();
    });
}

void Network::onIwdInterfacesAdded(const QDBusObjectPath &path, const InterfaceMap &ifaces)
{
    if (ifaces.contains(kIwdStation)) m_stations.insert(path.path(), ifaces.value(kIwdStation));
    if (ifaces.contains(kIwdNetwork)) m_networks.insert(path.path(), ifaces.value(kIwdNetwork));
    iwdRecompute();
}

void Network::onIwdInterfacesRemoved(const QDBusObjectPath &path, const QStringList &ifaces)
{
    if (ifaces.contains(kIwdStation)) m_stations.remove(path.path());
    if (ifaces.contains(kIwdNetwork)) m_networks.remove(path.path());
    iwdRecompute();
}

void Network::onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &, const QDBusMessage &msg)
{
    const QString path = msg.path();
    if (m_backend == QLatin1String("iwd")) {
        QHash<QString, QVariantMap> *table = iface == kIwdStation ? &m_stations : iface == kIwdNetwork ? &m_networks : nullptr;
        if (!table) return;
        QVariantMap &props = (*table)[path];
        for (auto it = changed.cbegin(); it != changed.cend(); ++it) props.insert(it.key(), it.value());
        iwdRecompute();
        if (iface == kIwdStation && changed.contains(QStringLiteral("ConnectedNetwork"))) iwdPollStrength();
    } else if (m_backend == QLatin1String("networkmanager") && path == kNMPath) {
        nmRefresh();
    }
}

void Network::iwdRecompute()
{
    m_wifiConnected = false;
    m_wifiSsid.clear();
    for (auto it = m_stations.cbegin(); it != m_stations.cend(); ++it) {
        const QString state = plain(it->value(QStringLiteral("State"))).toString();
        if (state != QLatin1String("connected") && state != QLatin1String("roaming")) continue;
        const QString net = plain(it->value(QStringLiteral("ConnectedNetwork"))).value<QDBusObjectPath>().path();
        m_wifiConnected = true;
        m_wifiSsid = plain(m_networks.value(net).value(QStringLiteral("Name"))).toString();
        break;
    }
    if (!m_wifiConnected) m_wifiStrength = -1;
    recompute();
}

void Network::iwdPollStrength()
{
    for (auto it = m_stations.cbegin(); it != m_stations.cend(); ++it) {
        const QString state = plain(it->value(QStringLiteral("State"))).toString();
        if (state != QLatin1String("connected")) continue;
        const QString station = it.key();
        const QString connected = plain(it->value(QStringLiteral("ConnectedNetwork"))).value<QDBusObjectPath>().path();
        QDBusMessage m = QDBusMessage::createMethodCall(kIwd, station, kIwdStation, QStringLiteral("GetOrderedNetworks"));
        auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(m, 3000), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, connected]() {
            const QDBusMessage reply = w->reply();
            w->deleteLater();
            if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return;
            const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
            arg.beginArray();
            while (!arg.atEnd()) {
                QDBusObjectPath p;
                qint16 strength = 0;
                arg.beginStructure();
                arg >> p >> strength;
                arg.endStructure();
                if (p.path() == connected) {
                    m_wifiStrength = dbmToPercent(strength / 100);
                    recompute(false);
                }
            }
            arg.endArray();
        });
        break;
    }
}

void Network::nmRefresh()
{
    getAll(m_bus, this, kNM, kNMPath, kNM, [this](const QVariantMap &props, bool ok) {
        m_nm = props;
        const uint state = plain(props.value(QStringLiteral("State"))).toUInt();
        const QString type = plain(props.value(QStringLiteral("PrimaryConnectionType"))).toString();
        const QString primary = plain(props.value(QStringLiteral("PrimaryConnection"))).value<QDBusObjectPath>().path();
        const bool connected = ok && state >= 50;
        m_nmWireless = connected && type == QLatin1String("802-11-wireless");
        if (!m_nmWireless || primary.isEmpty() || primary == QLatin1String("/")) {
            m_wifiConnected = false;
            m_wifiSsid.clear();
            m_wifiStrength = -1;
            const bool first = !m_loaded;
            m_loaded = true;
            recompute(!first);
            if (first) m_lastKind = m_kind, m_lastSsid = m_ssid;
            return;
        }
        getAll(m_bus, this, kNM, primary, QStringLiteral("org.freedesktop.NetworkManager.Connection.Active"),
               [this](const QVariantMap &active, bool) {
                   m_wifiConnected = true;
                   m_wifiSsid = plain(active.value(QStringLiteral("Id"))).toString();
                   const QString ap = plain(active.value(QStringLiteral("SpecificObject"))).value<QDBusObjectPath>().path();
                   auto finish = [this]() {
                       const bool first = !m_loaded;
                       m_loaded = true;
                       recompute(!first);
                       if (first) m_lastKind = m_kind, m_lastSsid = m_ssid;
                   };
                   if (ap.isEmpty() || ap == QLatin1String("/")) {
                       finish();
                       return;
                   }
                   getAll(m_bus, this, kNM, ap, QStringLiteral("org.freedesktop.NetworkManager.AccessPoint"),
                          [this, finish](const QVariantMap &apProps, bool ok) {
                              if (ok) {
                                  m_wifiStrength = int(plain(apProps.value(QStringLiteral("Strength"))).toUInt());
                                  const QByteArray ssid = plain(apProps.value(QStringLiteral("Ssid"))).toByteArray();
                                  if (!ssid.isEmpty()) m_wifiSsid = QString::fromUtf8(ssid);
                              }
                              finish();
                          });
               });
    });
}

void Network::pollWired()
{
    QString root = qEnvironmentVariable("SHADE_SYSFS_NET");
    if (root.isEmpty()) root = QStringLiteral("/sys/class/net");
    bool wired = false;
    const QDir dir(root);
    for (const QString &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        if (name == QLatin1String("lo")) continue;
        const QString base = root + u'/' + name;
        if (QFile::exists(base + QStringLiteral("/wireless")) || QFile::exists(base + QStringLiteral("/phy80211"))) continue;
        if (!QFile::exists(base + QStringLiteral("/device"))) continue; // virtual (bridges, veth, tun)
        QFile type(base + QStringLiteral("/type"));
        if (type.open(QIODevice::ReadOnly) && type.readAll().trimmed() != "1") continue; // not ethernet
        QFile carrier(base + QStringLiteral("/carrier"));
        QFile oper(base + QStringLiteral("/operstate"));
        const bool up = (oper.open(QIODevice::ReadOnly) && oper.readAll().trimmed() == "up") ||
                        (carrier.open(QIODevice::ReadOnly) && carrier.readAll().trimmed() == "1");
        if (up) {
            wired = true;
            break;
        }
    }
    if (wired != m_wired) {
        m_wired = wired;
        recompute();
    }
}

void Network::recompute(bool allowEvent)
{
    if (m_wired) {
        m_kind = Kind::Wired;
        m_ssid.clear();
        m_strength = -1;
    } else if (m_wifiConnected) {
        m_kind = Kind::Wireless;
        m_ssid = m_wifiSsid;
        m_strength = m_wifiStrength;
    } else {
        m_kind = Kind::Offline;
        m_ssid.clear();
        m_strength = -1;
    }
    emit changed();
    if (!m_loaded) return;
    if (m_kind == m_lastKind && m_ssid == m_lastSsid) return;
    const Kind before = m_lastKind;
    m_lastKind = m_kind;
    m_lastSsid = m_ssid;
    if (!allowEvent || m_quiet) return;
    QString text;
    switch (m_kind) {
    case Kind::Wired: text = QStringLiteral("Ethernet connected"); break;
    case Kind::Wireless: text = m_ssid.isEmpty() ? QStringLiteral("Wi-Fi connected") : QStringLiteral("Connected to %1").arg(m_ssid); break;
    case Kind::Offline: text = before == Kind::Wired ? QStringLiteral("Ethernet disconnected") : QStringLiteral("Disconnected"); break;
    }
    emit notable(Event{QStringLiteral("network"), icon(), text, 2, -1});
}

QString Network::icon() const
{
    switch (m_kind) {
    case Kind::Wired: return stats::icon(QStringLiteral("network-wired"));
    case Kind::Offline: return stats::icon(QStringLiteral("network-offline"));
    case Kind::Wireless: break;
    }
    const int s = m_strength;
    const int bars = s < 0 ? 3 : s < 20 ? 0 : s < 40 ? 1 : s < 60 ? 2 : s < 80 ? 3 : 4;
    return stats::icon(QStringLiteral("wifi-%1").arg(bars));
}

QString Network::describe() const
{
    switch (m_kind) {
    case Kind::Wired: return QStringLiteral("Ethernet");
    case Kind::Offline: return m_backend == QLatin1String("none") ? QStringLiteral("No network service") : QStringLiteral("Disconnected");
    case Kind::Wireless: break;
    }
    return m_ssid.isEmpty() ? QStringLiteral("Wi-Fi") : m_ssid;
}

// ===========================================================================
// Bluetooth (BlueZ)
// ===========================================================================

namespace
{
const QString kBluez = QStringLiteral("org.bluez");
const QString kAdapter = QStringLiteral("org.bluez.Adapter1");
const QString kDevice = QStringLiteral("org.bluez.Device1");
const QString kBattery = QStringLiteral("org.bluez.Battery1");
} // namespace

Bluetooth::Bluetooth(QDBusConnection bus, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
{
    m_watcher = new QDBusServiceWatcher(kBluez, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this, &Bluetooth::scan);
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this]() {
        m_adapters.clear();
        m_devices.clear();
        recompute();
    });
    m_bus.connect(kBluez, QStringLiteral("/"), kObjMgr, QStringLiteral("InterfacesAdded"), this,
                  SLOT(onInterfacesAdded(QDBusObjectPath, stats::InterfaceMap)));
    m_bus.connect(kBluez, QStringLiteral("/"), kObjMgr, QStringLiteral("InterfacesRemoved"), this,
                  SLOT(onInterfacesRemoved(QDBusObjectPath, QStringList)));
    // Every object's PropertiesChanged (adapters, devices, batteries): an
    // empty path matches all of them.
    m_bus.connect(kBluez, QString(), kProps, QStringLiteral("PropertiesChanged"), this,
                  SLOT(onPropertiesChanged(QString, QVariantMap, QStringList, QDBusMessage)));
    scan();
}

void Bluetooth::scan()
{
    managedObjects(m_bus, this, kBluez, [this](const ManagedObjects &objs, bool ok) {
        m_adapters.clear();
        m_devices.clear();
        if (ok) {
            for (auto it = objs.cbegin(); it != objs.cend(); ++it) onInterfacesAdded(it.key(), it.value());
        }
        const bool first = !m_loaded;
        m_loaded = true;
        if (first) {
            // The initial state is not an event.
            QSignalBlocker block(this);
            recompute();
        }
        recompute();
    });
}

void Bluetooth::onInterfacesAdded(const QDBusObjectPath &path, const InterfaceMap &ifaces)
{
    const QString p = path.path();
    if (ifaces.contains(kAdapter)) m_adapters.insert(p, plain(ifaces.value(kAdapter).value(QStringLiteral("Powered"))).toBool());
    if (ifaces.contains(kDevice)) {
        const QVariantMap d = ifaces.value(kDevice);
        Device &dev = m_devices[p];
        dev.alias = plain(d.value(QStringLiteral("Alias"), d.value(QStringLiteral("Name")))).toString();
        dev.connected = plain(d.value(QStringLiteral("Connected"))).toBool();
    }
    if (ifaces.contains(kBattery)) {
        m_devices[p].battery = int(plain(ifaces.value(kBattery).value(QStringLiteral("Percentage"))).toUInt());
    }
    if (m_loaded) recompute();
}

void Bluetooth::onInterfacesRemoved(const QDBusObjectPath &path, const QStringList &ifaces)
{
    const QString p = path.path();
    if (ifaces.contains(kAdapter)) m_adapters.remove(p);
    if (ifaces.contains(kDevice)) {
        const Device dev = m_devices.take(p);
        if (dev.connected && m_loaded) {
            emit notable(Event{QStringLiteral("bluetooth"), stats::icon(QStringLiteral("bluetooth-on")),
                             QStringLiteral("%1 disconnected").arg(dev.alias), 2, -1});
        }
    }
    if (m_loaded) recompute();
}

QString Bluetooth::deviceName(const QString &path) const
{
    const QString a = m_devices.value(path).alias;
    return a.isEmpty() ? QStringLiteral("Device") : a;
}

void Bluetooth::onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &, const QDBusMessage &msg)
{
    const QString path = msg.path();
    if (iface == kAdapter) {
        if (!changed.contains(QStringLiteral("Powered"))) return;
        const bool before = m_adapters.value(path, false);
        const bool now = plain(changed.value(QStringLiteral("Powered"))).toBool();
        m_adapters.insert(path, now);
        if (before != now && m_loaded) {
            emit notable(Event{QStringLiteral("bluetooth"), stats::icon(now ? QStringLiteral("bluetooth-on") : QStringLiteral("bluetooth-off")),
                             now ? QStringLiteral("Bluetooth on") : QStringLiteral("Bluetooth off"), 2, -1});
        }
        recompute();
    } else if (iface == kDevice) {
        Device &dev = m_devices[path];
        if (changed.contains(QStringLiteral("Alias"))) dev.alias = plain(changed.value(QStringLiteral("Alias"))).toString();
        if (changed.contains(QStringLiteral("Connected"))) {
            const bool now = plain(changed.value(QStringLiteral("Connected"))).toBool();
            if (now != dev.connected && m_loaded) {
                dev.connected = now;
                QString text = QStringLiteral("%1 %2").arg(deviceName(path), now ? QStringLiteral("connected") : QStringLiteral("disconnected"));
                if (now && dev.battery >= 0) text += QStringLiteral(" · %1%").arg(dev.battery);
                emit notable(Event{QStringLiteral("bluetooth"), stats::icon(now ? QStringLiteral("bluetooth-connected") : QStringLiteral("bluetooth-on")),
                                 text, 2, now ? dev.battery : -1});
            }
            dev.connected = now;
        }
        recompute();
    } else if (iface == kBattery) {
        if (changed.contains(QStringLiteral("Percentage"))) m_devices[path].battery = int(plain(changed.value(QStringLiteral("Percentage"))).toUInt());
        recompute();
    }
}

void Bluetooth::recompute()
{
    State s = State::Unavailable;
    if (!m_adapters.isEmpty()) {
        const bool powered = std::any_of(m_adapters.cbegin(), m_adapters.cend(), [](bool p) { return p; });
        s = powered ? State::On : State::Off;
        if (powered && std::any_of(m_devices.cbegin(), m_devices.cend(), [](const Device &d) { return d.connected; })) s = State::Connected;
    }
    m_state = s;
    emit changed();
}

QStringList Bluetooth::connectedDevices() const
{
    QStringList out;
    for (const Device &d : m_devices) {
        if (d.connected) out << (d.battery >= 0 ? QStringLiteral("%1 · %2%").arg(d.alias).arg(d.battery) : d.alias);
    }
    out.sort();
    return out;
}

QString Bluetooth::icon() const
{
    switch (m_state) {
    case State::Connected: return stats::icon(QStringLiteral("bluetooth-connected"));
    case State::On: return stats::icon(QStringLiteral("bluetooth-on"));
    case State::Off:
    case State::Unavailable: break;
    }
    return stats::icon(QStringLiteral("bluetooth-off"));
}

QString Bluetooth::describe() const
{
    switch (m_state) {
    case State::Unavailable: return QStringLiteral("Bluetooth unavailable");
    case State::Off: return QStringLiteral("Bluetooth off");
    case State::On: return QStringLiteral("Bluetooth on");
    case State::Connected: return connectedDevices().join(QStringLiteral(", "));
    }
    return {};
}

// ===========================================================================
// Volume (pactl, works with PulseAudio and PipeWire)
// ===========================================================================

Volume::Volume(const QString &pactl, int step, QObject *parent)
    : QObject(parent)
    , m_pactl(pactl.isEmpty() ? QStringLiteral("pactl") : pactl)
    , m_step(std::clamp(step, 1, 50))
{
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(40);
    connect(m_debounce, &QTimer::timeout, this, &Volume::refresh);
    m_restart = new QTimer(this);
    m_restart->setSingleShot(true);
    m_restart->setInterval(3000);
    connect(m_restart, &QTimer::timeout, this, &Volume::subscribe);
    subscribe();
    refresh();
}

Volume::~Volume()
{
    if (m_subscribe) {
        m_subscribe->disconnect(this);
        m_subscribe->kill();
        m_subscribe->waitForFinished(500);
    }
}

void Volume::subscribe()
{
    if (m_subscribe) m_subscribe->deleteLater();
    m_subscribe = new QProcess(this);
    m_subscribe->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_subscribe, &QProcess::readyReadStandardOutput, this, [this]() {
        while (m_subscribe->canReadLine()) {
            const QByteArray line = m_subscribe->readLine();
            if (line.contains("sink") || line.contains("server")) m_debounce->start();
        }
    });
    connect(m_subscribe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (m_available) {
            m_available = false;
            emit changed();
        }
        m_restart->start();
    });
    connect(m_subscribe, &QProcess::finished, this, [this]() { m_restart->start(); });
    m_subscribe->start(m_pactl, {QStringLiteral("subscribe")});
}

void Volume::run(const QStringList &args, std::function<void(int, const QByteArray &)> done)
{
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [p, done](int code, QProcess::ExitStatus st) {
        done(st == QProcess::NormalExit ? code : -1, p->readAllStandardOutput());
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, this, [p, done](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            done(-1, {});
            p->deleteLater();
        }
    });
    p->start(m_pactl, args);
}

void Volume::refresh()
{
    run({QStringLiteral("get-sink-volume"), QStringLiteral("@DEFAULT_SINK@")}, [this](int code, const QByteArray &out) {
        static const QRegularExpression pct(QStringLiteral("(\\d+)%"));
        const auto m = pct.match(QString::fromUtf8(out));
        if (code != 0 || !m.hasMatch()) {
            if (m_available) {
                m_available = false;
                emit changed();
            }
            return;
        }
        const int level = m.captured(1).toInt();
        run({QStringLiteral("get-sink-mute"), QStringLiteral("@DEFAULT_SINK@")}, [this, level](int code2, const QByteArray &out2) {
            const bool muted = code2 == 0 && out2.contains("yes");
            const bool was = m_loaded;
            const bool differs = level != m_level || muted != m_muted || !m_available;
            m_available = true;
            m_loaded = true;
            m_level = level;
            m_muted = muted;
            if (!differs) return;
            emit changed();
            if (was && QDateTime::currentMSecsSinceEpoch() > m_ownChangeUntil) {
                emit notable(Event{QStringLiteral("volume"), icon(), muted ? QStringLiteral("Muted") : QStringLiteral("Volume"), 2, muted ? 0 : level});
            }
        });
    });
}

void Volume::adjust(int steps)
{
    if (!m_available || steps == 0) return;
    const int target = std::clamp(m_level + steps * m_step, 0, 100);
    m_ownChangeUntil = QDateTime::currentMSecsSinceEpoch() + 1500;
    if (target == m_level) return;
    m_level = target; // optimistic: the widget follows the scroll at once
    emit changed();
    run({QStringLiteral("set-sink-volume"), QStringLiteral("@DEFAULT_SINK@"), QStringLiteral("%1%").arg(target)},
        [this](int, const QByteArray &) { m_debounce->start(); });
}

void Volume::toggleMute()
{
    if (!m_available) return;
    m_ownChangeUntil = QDateTime::currentMSecsSinceEpoch() + 1500;
    run({QStringLiteral("set-sink-mute"), QStringLiteral("@DEFAULT_SINK@"), QStringLiteral("toggle")},
        [this](int, const QByteArray &) { m_debounce->start(); });
}

QString Volume::icon() const
{
    if (!m_available || m_muted) return stats::icon(QStringLiteral("volume-muted"));
    const int bars = m_level <= 0 ? 0 : m_level < 34 ? 1 : m_level < 67 ? 2 : 3;
    return stats::icon(QStringLiteral("volume-%1").arg(bars));
}

} // namespace stats
