#include "mpris.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDateTime>
#include <QDebug>
#include <QTimer>

namespace media
{

namespace
{
const QString kPrefix = QStringLiteral("org.mpris.MediaPlayer2.");
const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayer = QStringLiteral("org.mpris.MediaPlayer2.Player");
const QString kRoot = QStringLiteral("org.mpris.MediaPlayer2");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");

QVariant unwrap(const QVariant &v)
{
    if (v.userType() == qMetaTypeId<QDBusVariant>()) return v.value<QDBusVariant>().variant();
    return v;
}

QStringList toStringList(const QVariant &v)
{
    const QVariant u = unwrap(v);
    if (u.userType() == qMetaTypeId<QDBusArgument>()) {
        QStringList out;
        const QDBusArgument arg = u.value<QDBusArgument>();
        arg >> out;
        return out;
    }
    if (u.canConvert<QStringList>()) return u.toStringList();
    return {u.toString()};
}

bool ignored(const QString &service)
{
    // playerctld mirrors other players; binding to it would duplicate them.
    return service == QLatin1String("org.mpris.MediaPlayer2.playerctld");
}
} // namespace

Mpris::Mpris(QDBusConnection bus, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
{
    m_poll = new QTimer(this);
    m_poll->setInterval(1000);
    connect(m_poll, &QTimer::timeout, this, &Mpris::pollPosition);

    m_bus.connect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"),
                  QStringLiteral("NameOwnerChanged"), this, SLOT(onNameOwnerChanged(QString, QString, QString)));
    // All players' property changes and seeks; matched to a player by the
    // sender's unique name.
    m_bus.connect(QString(), kPath, kProps, QStringLiteral("PropertiesChanged"), this,
                  SLOT(onPropertiesChanged(QString, QVariantMap, QStringList, QDBusMessage)));
    m_bus.connect(QString(), kPath, kPlayer, QStringLiteral("Seeked"), this, SLOT(onSeeked(qint64, QDBusMessage)));

    QDBusMessage list = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                                       QStringLiteral("org.freedesktop.DBus"), QStringLiteral("ListNames"));
    auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(list), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w]() {
        QDBusPendingReply<QStringList> r = *w;
        w->deleteLater();
        if (r.isError()) return;
        for (const QString &name : r.value()) {
            if (!name.startsWith(kPrefix) || ignored(name)) continue;
            QDBusMessage owner = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                                                QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
            owner << name;
            auto *ow = new QDBusPendingCallWatcher(m_bus.asyncCall(owner), this);
            connect(ow, &QDBusPendingCallWatcher::finished, this, [this, ow, name]() {
                QDBusPendingReply<QString> o = *ow;
                ow->deleteLater();
                if (!o.isError()) addPlayer(name, o.value());
            });
        }
    });
}

void Mpris::onNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner)
{
    if (!name.startsWith(kPrefix) || ignored(name)) return;
    if (!oldOwner.isEmpty()) removePlayer(name);
    if (!newOwner.isEmpty()) addPlayer(name, newOwner);
}

void Mpris::addPlayer(const QString &service, const QString &owner)
{
    Player &p = m_players[service];
    p.service = service;
    p.owner = owner;
    fetch(service);
}

void Mpris::removePlayer(const QString &service)
{
    m_players.remove(service);
    if (service == m_bound) m_bound.clear();
    select();
}

Mpris::Player *Mpris::playerByOwner(const QString &owner)
{
    for (Player &p : m_players) {
        if (p.owner == owner || p.service == owner) return &p;
    }
    return nullptr;
}

void Mpris::fetch(const QString &service)
{
    QDBusMessage m = QDBusMessage::createMethodCall(service, kPath, kProps, QStringLiteral("GetAll"));
    m << kPlayer;
    auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(m, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, service]() {
        QDBusPendingReply<QVariantMap> r = *w;
        w->deleteLater();
        auto it = m_players.find(service);
        if (it == m_players.end()) return;
        if (r.isError()) {
            // Not (yet) a usable player; keep it, it may come to life.
            return;
        }
        it->props = r.value();
        it->loaded = true;
        it->positionUs = unwrap(it->props.value(QStringLiteral("Position"))).toLongLong();
        it->positionClock.restart();
        if (unwrap(it->props.value(QStringLiteral("PlaybackStatus"))).toString() == QLatin1String("Playing")) {
            it->lastPlaying = QDateTime::currentMSecsSinceEpoch();
        }
        // Identity lives on the root interface.
        QDBusMessage idm = QDBusMessage::createMethodCall(service, kPath, kProps, QStringLiteral("Get"));
        idm << kRoot << QStringLiteral("Identity");
        auto *iw = new QDBusPendingCallWatcher(m_bus.asyncCall(idm, 2000), this);
        connect(iw, &QDBusPendingCallWatcher::finished, this, [this, iw, service]() {
            QDBusPendingReply<QDBusVariant> ir = *iw;
            iw->deleteLater();
            auto pit = m_players.find(service);
            if (pit != m_players.end() && !ir.isError()) {
                pit->props.insert(QStringLiteral("__identity"), ir.value().variant().toString());
                if (service == m_bound) publish();
            }
        });
        select();
    });
}

void Mpris::onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated, const QDBusMessage &msg)
{
    if (iface != kPlayer) return;
    Player *p = playerByOwner(msg.service());
    if (!p) return;
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) p->props.insert(it.key(), it.value());
    if (changed.contains(QStringLiteral("PlaybackStatus"))) {
        if (unwrap(changed.value(QStringLiteral("PlaybackStatus"))).toString() == QLatin1String("Playing")) {
            p->lastPlaying = QDateTime::currentMSecsSinceEpoch();
        }
        // Anchor extrapolation at the current estimate when status flips.
        pollPosition();
    }
    if (changed.contains(QStringLiteral("Metadata"))) {
        p->positionUs = 0;
        p->positionClock.restart();
    }
    if (!invalidated.isEmpty()) fetch(p->service);
    select();
}

void Mpris::onSeeked(qint64 position, const QDBusMessage &msg)
{
    Player *p = playerByOwner(msg.service());
    if (!p) return;
    p->positionUs = position;
    p->positionClock.restart();
    if (p->service == m_bound) emit positionChanged();
}

void Mpris::setPolling(bool on)
{
    if (on) {
        m_poll->start();
        pollPosition();
    } else {
        m_poll->stop();
    }
}

void Mpris::pollPosition()
{
    if (m_bound.isEmpty()) return;
    const QString service = m_bound;
    QDBusMessage m = QDBusMessage::createMethodCall(service, kPath, kProps, QStringLiteral("Get"));
    m << kPlayer << QStringLiteral("Position");
    auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(m, 2000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, service]() {
        QDBusPendingReply<QDBusVariant> r = *w;
        w->deleteLater();
        auto it = m_players.find(service);
        if (it == m_players.end() || r.isError()) return;
        it->positionUs = r.value().variant().toLongLong();
        it->positionClock.restart();
        if (service == m_bound) emit positionChanged();
    });
}

qint64 Mpris::positionUs() const
{
    auto it = m_players.constFind(m_bound);
    if (it == m_players.constEnd()) return 0;
    qint64 pos = it->positionUs;
    if (m_state.status == QLatin1String("Playing") && it->positionClock.isValid()) {
        pos += qint64(it->positionClock.elapsed() * 1000 * m_state.rate);
    }
    if (m_state.track.lengthUs > 0) pos = std::min(pos, m_state.track.lengthUs);
    return std::max<qint64>(0, pos);
}

void Mpris::select()
{
    // Playing beats paused beats stopped; then most recently playing; then
    // name, so the choice never depends on bus enumeration order.
    QString best;
    int bestRank = -1;
    qint64 bestRecent = -1;
    for (const Player &p : m_players) {
        if (!p.loaded) continue;
        const QString status = unwrap(p.props.value(QStringLiteral("PlaybackStatus"))).toString();
        const int rank = status == QLatin1String("Playing") ? 2 : status == QLatin1String("Paused") ? 1 : 0;
        const bool better = rank > bestRank || (rank == bestRank && p.lastPlaying > bestRecent) ||
                            (rank == bestRank && p.lastPlaying == bestRecent && p.service < best);
        if (better) {
            best = p.service;
            bestRank = rank;
            bestRecent = p.lastPlaying;
        }
    }
    // Stickiness: keep a paused bound player unless another one is playing.
    if (!m_bound.isEmpty() && m_players.contains(m_bound) && bestRank < 2) best = m_bound;
    m_bound = best;
    publish();
}

Track Mpris::parseMetadata(const QVariant &metadata)
{
    QVariantMap map;
    const QVariant v = unwrap(metadata);
    if (v.userType() == qMetaTypeId<QDBusArgument>()) v.value<QDBusArgument>() >> map;
    else map = v.toMap();
    Track t;
    t.title = unwrap(map.value(QStringLiteral("xesam:title"))).toString();
    t.artist = toStringList(map.value(QStringLiteral("xesam:artist"))).join(QStringLiteral(", "));
    t.album = unwrap(map.value(QStringLiteral("xesam:album"))).toString();
    t.artUrl = unwrap(map.value(QStringLiteral("mpris:artUrl"))).toString();
    t.url = unwrap(map.value(QStringLiteral("xesam:url"))).toString();
    const QVariant tid = unwrap(map.value(QStringLiteral("mpris:trackid")));
    t.trackId = tid.userType() == qMetaTypeId<QDBusObjectPath>() ? tid.value<QDBusObjectPath>().path() : tid.toString();
    t.lengthUs = unwrap(map.value(QStringLiteral("mpris:length"))).toLongLong();
    return t;
}

void Mpris::publish()
{
    PlayerState s;
    auto it = m_players.constFind(m_bound);
    if (it != m_players.constEnd()) {
        const QVariantMap &p = it->props;
        s.service = it->service;
        s.identity = p.value(QStringLiteral("__identity")).toString();
        if (s.identity.isEmpty()) s.identity = it->service.mid(kPrefix.size()).section(u'.', 0, 0);
        s.track = parseMetadata(p.value(QStringLiteral("Metadata")));
        s.status = unwrap(p.value(QStringLiteral("PlaybackStatus"), QStringLiteral("Stopped"))).toString();
        s.hasShuffle = p.contains(QStringLiteral("Shuffle"));
        s.shuffle = unwrap(p.value(QStringLiteral("Shuffle"))).toBool();
        s.hasLoop = p.contains(QStringLiteral("LoopStatus"));
        s.loop = unwrap(p.value(QStringLiteral("LoopStatus"), QStringLiteral("None"))).toString();
        s.canControl = unwrap(p.value(QStringLiteral("CanControl"), true)).toBool();
        s.canNext = s.canControl && unwrap(p.value(QStringLiteral("CanGoNext"))).toBool();
        s.canPrevious = s.canControl && unwrap(p.value(QStringLiteral("CanGoPrevious"))).toBool();
        s.canPlay = s.canControl && unwrap(p.value(QStringLiteral("CanPlay"))).toBool();
        s.canPause = s.canControl && unwrap(p.value(QStringLiteral("CanPause"))).toBool();
        s.canSeek = s.canControl && unwrap(p.value(QStringLiteral("CanSeek"))).toBool() && s.track.lengthUs > 0;
        s.rate = unwrap(p.value(QStringLiteral("Rate"), 1.0)).toDouble();
        if (s.rate <= 0) s.rate = 1.0;
    }
    const bool sameService = s.service == m_state.service;
    const bool trackDiffers = !(s.track == m_state.track);
    const bool changed = !sameService || trackDiffers || s.status != m_state.status || s.shuffle != m_state.shuffle ||
                         s.loop != m_state.loop || s.canNext != m_state.canNext || s.canPrevious != m_state.canPrevious ||
                         s.canPlay != m_state.canPlay || s.canPause != m_state.canPause || s.canSeek != m_state.canSeek ||
                         s.identity != m_state.identity || s.hasShuffle != m_state.hasShuffle || s.hasLoop != m_state.hasLoop;
    const bool newTrack = !s.service.isEmpty() && !s.track.title.isEmpty() &&
                          (s.track.title != m_state.track.title || s.track.artist != m_state.track.artist || !sameService);
    m_state = s;
    if (changed) emit stateChanged();
    if (newTrack) emit trackChanged();
    emit positionChanged();
}

void Mpris::call(const QString &method, const QVariantList &args)
{
    if (m_bound.isEmpty()) return;
    QDBusMessage m = QDBusMessage::createMethodCall(m_bound, kPath, kPlayer, method);
    m.setArguments(args);
    m_bus.asyncCall(m, 3000);
}

void Mpris::setProperty(const QString &name, const QVariant &value)
{
    if (m_bound.isEmpty()) return;
    QDBusMessage m = QDBusMessage::createMethodCall(m_bound, kPath, kProps, QStringLiteral("Set"));
    m << kPlayer << name << QVariant::fromValue(QDBusVariant(value));
    m_bus.asyncCall(m, 3000);
}

void Mpris::playPause() { call(QStringLiteral("PlayPause")); }
void Mpris::next() { call(QStringLiteral("Next")); }

void Mpris::previous()
{
    if (positionUs() > 3'000'000 && m_state.canSeek) seekTo(0);
    else call(QStringLiteral("Previous"));
}

void Mpris::seekTo(qint64 us)
{
    if (!m_state.canSeek) return;
    us = std::clamp<qint64>(us, 0, m_state.track.lengthUs);
    if (!m_state.track.trackId.isEmpty()) {
        call(QStringLiteral("SetPosition"), {QVariant::fromValue(QDBusObjectPath(m_state.track.trackId)), QVariant::fromValue<qint64>(us)});
    } else {
        call(QStringLiteral("Seek"), {QVariant::fromValue<qint64>(us - positionUs())});
    }
    // Optimistic: show the new position right away.
    auto it = m_players.find(m_bound);
    if (it != m_players.end()) {
        it->positionUs = us;
        it->positionClock.restart();
    }
    emit positionChanged();
}

void Mpris::setShuffle(bool on) { setProperty(QStringLiteral("Shuffle"), on); }

void Mpris::cycleLoop()
{
    const QString next = m_state.loop == QLatin1String("None") ? QStringLiteral("Playlist")
                         : m_state.loop == QLatin1String("Playlist") ? QStringLiteral("Track")
                                                                      : QStringLiteral("None");
    setProperty(QStringLiteral("LoopStatus"), next);
}

} // namespace media
