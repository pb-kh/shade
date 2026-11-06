#pragma once

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

class QDBusMessage;
class QDBusServiceWatcher;
class QProcess;
class QTimer;

namespace stats
{

using InterfaceMap = QMap<QString, QVariantMap>;
using ManagedObjects = QMap<QDBusObjectPath, InterfaceMap>;
void registerDBusTypes();

// The bus that hosts system services. SHADE_STATS_BUS=session (or the
// plugin's `system_bus = "session"`) points the plugin at mock services.
QDBusConnection systemBus(const QString &configured);

// A notable change worth a peek.
struct Event
{
    QString kind;   // "battery", "network", "bluetooth", "volume"
    QString icon;   // icon resource path
    QString text;
    int priority = 2;
    int value = -1; // optional 0..100 (drawn as a level bar)
};

// ---------------------------------------------------------------------------

class Battery : public QObject
{
    Q_OBJECT

public:
    enum class State { Unknown, Charging, Discharging, Empty, Full, PendingCharge, PendingDischarge };

    Battery(QDBusConnection bus, QList<int> lowThresholds, QObject *parent = nullptr);

    bool present() const { return m_present; }
    double percentage() const { return m_percentage; }
    State state() const { return m_state; }
    bool charging() const { return m_state == State::Charging || m_state == State::PendingCharge || m_state == State::Full; }
    QString icon() const;
    QString describe() const;

signals:
    void changed();
    void notable(const stats::Event &e);

private slots:
    void onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);

private:
    void refresh();
    void apply(const QVariantMap &props, bool initial);

    QDBusConnection m_bus;
    QList<int> m_low;
    QDBusServiceWatcher *m_watcher;
    bool m_present = false;
    bool m_loaded = false;
    double m_percentage = 0;
    State m_state = State::Unknown;
    int m_lastLowWarned = 101;
};

// ---------------------------------------------------------------------------

class Network : public QObject
{
    Q_OBJECT

public:
    enum class Kind { Offline, Wired, Wireless };

    Network(QDBusConnection bus, const QString &backend, QObject *parent = nullptr);

    Kind kind() const { return m_kind; }
    QString ssid() const { return m_ssid; }
    int strength() const { return m_strength; } // 0..100, -1 unknown
    QString backend() const { return m_backend; }
    QString icon() const;
    QString describe() const;

    void setActive(bool active);

signals:
    void changed();
    void notable(const stats::Event &e);

private slots:
    void onIwdInterfacesAdded(const QDBusObjectPath &path, const stats::InterfaceMap &ifaces);
    void onIwdInterfacesRemoved(const QDBusObjectPath &path, const QStringList &ifaces);
    void onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated, const QDBusMessage &msg);

private:
    void chooseBackend();
    void iwdScan();
    void iwdRecompute();
    void iwdPollStrength();
    void nmRefresh();
    void pollWired();
    void recompute(bool allowEvent = true);

    QDBusConnection m_bus;
    QString m_requested;
    QString m_backend; // "iwd", "networkmanager", "none"
    QDBusServiceWatcher *m_watcher = nullptr;
    QTimer *m_wiredTimer;
    QTimer *m_strengthTimer;
    bool m_active = false;
    bool m_loaded = false;
    bool m_quiet = false;

    // iwd
    QHash<QString, QVariantMap> m_stations; // path -> Station props
    QHash<QString, QVariantMap> m_networks; // path -> Network props
    // networkmanager
    QVariantMap m_nm;
    QString m_nmSsid;
    int m_nmStrength = -1;
    bool m_nmWireless = false;

    bool m_wired = false;
    bool m_wifiConnected = false;
    QString m_wifiSsid;
    int m_wifiStrength = -1;

    Kind m_kind = Kind::Offline;
    QString m_ssid;
    int m_strength = -1;
    Kind m_lastKind = Kind::Offline;
    QString m_lastSsid;
};

// ---------------------------------------------------------------------------

class Bluetooth : public QObject
{
    Q_OBJECT

public:
    enum class State { Unavailable, Off, On, Connected };

    explicit Bluetooth(QDBusConnection bus, QObject *parent = nullptr);

    State state() const { return m_state; }
    QStringList connectedDevices() const;
    QString icon() const;
    QString describe() const;

signals:
    void changed();
    void notable(const stats::Event &e);

private slots:
    void onInterfacesAdded(const QDBusObjectPath &path, const stats::InterfaceMap &ifaces);
    void onInterfacesRemoved(const QDBusObjectPath &path, const QStringList &ifaces);
    void onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated, const QDBusMessage &msg);

private:
    struct Device
    {
        QString alias;
        bool connected = false;
        int battery = -1;
    };
    void scan();
    void recompute();
    QString deviceName(const QString &path) const;

    QDBusConnection m_bus;
    QDBusServiceWatcher *m_watcher;
    QHash<QString, bool> m_adapters; // path -> powered
    QHash<QString, Device> m_devices;
    bool m_loaded = false;
    State m_state = State::Unavailable;
};

// ---------------------------------------------------------------------------

class Volume : public QObject
{
    Q_OBJECT

public:
    Volume(const QString &pactl, int step, QObject *parent = nullptr);
    ~Volume() override;

    bool available() const { return m_available; }
    int level() const { return m_level; }
    bool muted() const { return m_muted; }
    QString icon() const;

    // Change by `steps` (positive = louder), clamped to 0..100.
    void adjust(int steps);
    void toggleMute();

signals:
    void changed();
    void notable(const stats::Event &e);

private:
    void subscribe();
    void refresh();
    void run(const QStringList &args, std::function<void(int, const QByteArray &)> done);

    QString m_pactl;
    int m_step;
    QProcess *m_subscribe = nullptr;
    QTimer *m_debounce;
    QTimer *m_restart;
    bool m_available = false;
    bool m_loaded = false;
    int m_level = 0;
    bool m_muted = false;
    qint64 m_ownChangeUntil = 0;
    int m_pending = 0;
};

} // namespace stats

Q_DECLARE_METATYPE(stats::InterfaceMap)
Q_DECLARE_METATYPE(stats::ManagedObjects)
Q_DECLARE_METATYPE(stats::Event)
