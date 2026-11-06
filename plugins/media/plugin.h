#pragma once

#include "art.h"
#include "mpris.h"

#include <shade/sdk.h>

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QVariantMap>

namespace media
{

// MPRIS media controls. One Mpris watcher and art loader shared by every
// window's pane; peeks the new track's title on track change (expand on
// hover morphs the peek into the full widget).
class MediaPlugin : public QObject, public shade::Plugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ShadePlugin_iid FILE "meta.json")
    Q_INTERFACES(shade::Plugin)

public:
    bool load(shade::Host *host) override;
    void unload() override;

    Mpris *mpris() const { return m_mpris; }
    QImage art() const { return m_art; }

signals:
    void artChanged(const QImage &art);

private:
    void onState();
    void onTrack();

    shade::Host *m_host = nullptr;
    QVariantMap m_cfg;
    Mpris *m_mpris = nullptr;
    ArtLoader *m_loader = nullptr;
    QImage m_art;
    QString m_artUrl;
    QElapsedTimer m_since;
    bool m_peekOnTrack = true;
    bool m_hadSession = false;
    QList<int> m_handlers;
};

} // namespace media
