#include "plugin.h"

#include "pane.h"

#include <QDBusConnection>

namespace media
{

bool MediaPlugin::load(shade::Host *host)
{
    m_host = host;
    m_cfg = host->config();
    m_peekOnTrack = m_cfg.value(QStringLiteral("peek_on_track_change"), true).toBool();
    m_since.start();

    m_mpris = new Mpris(QDBusConnection::sessionBus(), this);
    m_loader = new ArtLoader(this);
    connect(m_mpris, &Mpris::stateChanged, this, &MediaPlugin::onState);
    connect(m_mpris, &Mpris::trackChanged, this, &MediaPlugin::onTrack);
    connect(m_loader, &ArtLoader::ready, this, [this](const QString &url, const QImage &img) {
        if (url != m_artUrl) return;
        m_art = img;
        emit artChanged(img);
    });

    shade::Bus *bus = host->bus();
    m_handlers << bus->handle(QString::fromLatin1(shade::sig::WidgetRequest), [this](const shade::Signal &s) {
        auto *pane = new MediaPane(this);
        m_host->bus()->send(QString::fromLatin1(shade::kHost), QString::fromLatin1(shade::sig::Widget),
                            QVariantMap{{QStringLiteral("widget"), QVariant::fromValue<QObject *>(pane)}}, s.window);
    });
    m_handlers << bus->handle(QString::fromLatin1(shade::sig::Activate), [this](const shade::Signal &) { m_mpris->setPolling(true); });
    m_handlers << bus->handle(QString::fromLatin1(shade::sig::Deactivate), [this](const shade::Signal &) { m_mpris->setPolling(false); });
    // Remote control over the bus: `shadectl signal media control '{"action":"next"}'`.
    m_handlers << bus->handle(QStringLiteral("control"), [this](const shade::Signal &s) {
        const QString a = s.payload.value(QStringLiteral("action")).toString();
        if (a == QLatin1String("play-pause")) m_mpris->playPause();
        else if (a == QLatin1String("next")) m_mpris->next();
        else if (a == QLatin1String("previous")) m_mpris->previous();
        else if (a == QLatin1String("shuffle")) m_mpris->setShuffle(!m_mpris->state().shuffle);
        else if (a == QLatin1String("loop")) m_mpris->cycleLoop();
    });
    return true;
}

void MediaPlugin::unload()
{
    for (int id : m_handlers) m_host->bus()->unhandle(id);
    m_handlers.clear();
}

void MediaPlugin::onState()
{
    const PlayerState &st = m_mpris->state();
    const QString url = ArtLoader::resolve(st.track.artUrl, st.track.url);
    if (url != m_artUrl) {
        m_artUrl = url;
        m_loader->load(url);
    }
    if (st.service.isEmpty() && m_hadSession) {
        m_host->bus()->send(QString::fromLatin1(shade::kHost), QString::fromLatin1(shade::sig::DismissPeek),
                            QVariantMap{{QStringLiteral("id"), QStringLiteral("track")}});
    }
    m_hadSession = !st.service.isEmpty();
}

void MediaPlugin::onTrack()
{
    // Players already running at startup are state, not news.
    if (!m_peekOnTrack || m_since.elapsed() < 2000) return;
    const PlayerState &st = m_mpris->state();
    if (st.status != QLatin1String("Playing")) return;
    const QVariantMap data{{QStringLiteral("title"), st.track.title},
                           {QStringLiteral("artist"), st.track.artist.isEmpty() ? st.identity : st.track.artist}};
    m_host->bus()->send(QString::fromLatin1(shade::kHost), QString::fromLatin1(shade::sig::Peek),
                        shade::peekPayload(QStringLiteral("track"), data, shade::PeekPriority::Medium,
                                           QString::fromLatin1(shade::kExpandHover),
                                           m_cfg.value(QStringLiteral("peek_timeout_ms"), -1).toInt()));
}

} // namespace media
