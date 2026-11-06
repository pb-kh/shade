#pragma once

#include <QDBusConnection>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

class QDBusMessage;
class QTimer;

namespace media
{

struct Track
{
    QString title;
    QString artist;
    QString album;
    QString artUrl;
    QString url;      // xesam:url (used for the YouTube thumbnail fallback)
    QString trackId;  // mpris:trackid
    qint64 lengthUs = 0;

    bool operator==(const Track &o) const
    {
        return title == o.title && artist == o.artist && album == o.album && artUrl == o.artUrl && url == o.url &&
               trackId == o.trackId && lengthUs == o.lengthUs;
    }
};

struct PlayerState
{
    QString service; // bus name; empty = no session
    QString identity;
    Track track;
    QString status = QStringLiteral("Stopped"); // Playing / Paused / Stopped
    bool shuffle = false;
    QString loop = QStringLiteral("None"); // None / Track / Playlist
    bool hasShuffle = false;
    bool hasLoop = false;
    bool canNext = false, canPrevious = false, canPlay = false, canPause = false, canSeek = false, canControl = false;
    double rate = 1.0;
};

// Watches every MPRIS player on the session bus and binds to the most
// relevant one: a playing player wins; otherwise the one that played most
// recently; ties break on bus name. Proxies (playerctld) are ignored.
class Mpris : public QObject
{
    Q_OBJECT

public:
    explicit Mpris(QDBusConnection bus, QObject *parent = nullptr);

    const PlayerState &state() const { return m_state; }
    bool hasSession() const { return !m_state.service.isEmpty(); }
    QStringList players() const { return m_players.keys(); }
    // Position, extrapolated while playing.
    qint64 positionUs() const;

    // Poll the position every second (only while the pane is visible).
    void setPolling(bool on);

    void playPause();
    void next();
    void previous(); // restarts the track when past 3 s (MC-05)
    void seekTo(qint64 us);
    void setShuffle(bool on);
    void cycleLoop();

signals:
    void stateChanged();
    void trackChanged(); // a different track on the bound player
    void positionChanged();

private slots:
    void onNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);
    void onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated, const QDBusMessage &msg);
    void onSeeked(qint64 position, const QDBusMessage &msg);

private:
    struct Player
    {
        QString service;
        QString owner;
        QVariantMap props;
        qint64 lastPlaying = 0; // ms since epoch
        qint64 positionUs = 0;
        QElapsedTimer positionClock;
        bool loaded = false;
    };

    void addPlayer(const QString &service, const QString &owner);
    void removePlayer(const QString &service);
    void fetch(const QString &service);
    void pollPosition();
    void select();
    void publish();
    void call(const QString &method, const QVariantList &args = {});
    void setProperty(const QString &name, const QVariant &value);
    static Track parseMetadata(const QVariant &metadata);
    Player *playerByOwner(const QString &owner);

    QDBusConnection m_bus;
    QHash<QString, Player> m_players;
    QString m_bound;
    PlayerState m_state;
    QTimer *m_poll;
};

} // namespace media
