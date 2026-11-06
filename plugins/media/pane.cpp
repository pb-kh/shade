#include "pane.h"

#include "mpris.h"
#include "plugin.h"

#include <shade/theme.h>

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <cmath>

namespace media
{

namespace
{
constexpr int kWidth = 380;
constexpr int kHeight = 128;
constexpr int kIdleWidth = 230;
constexpr int kIdleHeight = 44;

QString icon(const QString &name) { return QStringLiteral(":/shade/media/icons/%1.svg").arg(name); }

QFont timeFont()
{
    QFont f = shade::Theme::font(11, QFont::Medium);
    f.setFeature(QFont::Tag("tnum"), 1); // digits of equal width
    return f;
}
} // namespace

MediaPane::MediaPane(MediaPlugin *plugin, QWidget *parent)
    : shade::PaneWidget(parent)
    , m_plugin(plugin)
{
    setMouseTracking(true);
    auto make = [this](const QString &name, int size) {
        auto *b = new shade::IconButton(icon(name), size, this);
        b->setPadding(6);
        return b;
    };
    m_shuffle = make(QStringLiteral("shuffle-off"), 15);
    m_prev = make(QStringLiteral("media-skip-backward"), 16);
    m_play = make(QStringLiteral("play"), 20);
    m_next = make(QStringLiteral("media-skip-forward"), 16);
    m_loop = make(QStringLiteral("loop-none"), 15);
    m_shuffle->setObjectName(QStringLiteral("shuffle"));
    m_prev->setObjectName(QStringLiteral("previous"));
    m_play->setObjectName(QStringLiteral("play"));
    m_next->setObjectName(QStringLiteral("next"));
    m_loop->setObjectName(QStringLiteral("loop"));
    for (shade::IconButton *b : {m_shuffle, m_prev, m_play, m_next, m_loop}) b->icon()->setDuration(360);

    Mpris *mpris = plugin->mpris();
    connect(m_play, &shade::IconButton::clicked, mpris, &Mpris::playPause);
    connect(m_prev, &shade::IconButton::clicked, mpris, &Mpris::previous);
    connect(m_next, &shade::IconButton::clicked, mpris, &Mpris::next);
    connect(m_shuffle, &shade::IconButton::clicked, mpris, [mpris]() { mpris->setShuffle(!mpris->state().shuffle); });
    connect(m_loop, &shade::IconButton::clicked, mpris, &Mpris::cycleLoop);
    connect(mpris, &Mpris::stateChanged, this, &MediaPane::refresh);
    connect(mpris, &Mpris::positionChanged, this, qOverload<>(&QWidget::update));
    connect(plugin, &MediaPlugin::artChanged, this, &MediaPane::setArt);

    m_note = new shade::MorphIcon(icon(QStringLiteral("note")), this);

    m_fade = new QTimer(this);
    m_fade->setInterval(16);
    connect(m_fade, &QTimer::timeout, this, [this]() {
        m_artFade = std::min(1.0, m_artFade + 16.0 / 260.0);
        if (m_artFade >= 1.0) {
            m_fade->stop();
            m_artPrev = {};
            m_bgPrev = {};
        }
        update();
        requestPeekRepaint();
    });
    // The seek bar advances smoothly while visible and playing.
    m_progress = new QTimer(this);
    m_progress->setInterval(250);
    connect(m_progress, &QTimer::timeout, this, qOverload<>(&QWidget::update));

    setArt(plugin->art());
    refresh();
}

QSize MediaPane::sizeHint() const
{
    if (!m_plugin || !m_plugin->mpris()->hasSession()) return QSize(kIdleWidth, kIdleHeight);
    return QSize(kWidth, kHeight);
}

QRectF MediaPane::seekRect() const
{
    const QFontMetrics fm(timeFont());
    const int tw = fm.horizontalAdvance(QStringLiteral("0:00:00")) + 2;
    return QRectF(92 + tw + 8, height() - 25, width() - 92 - 16 - 2 * (tw + 8), 4);
}

QString MediaPane::formatTime(qint64 us, bool hours)
{
    const qint64 s = std::max<qint64>(0, us / 1'000'000);
    if (hours) return QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QChar(u'0')).arg(s % 60, 2, 10, QChar(u'0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar(u'0'));
}

QString MediaPane::elapsedText() const
{
    if (!m_plugin) return {};
    const PlayerState &st = m_plugin->mpris()->state();
    const qint64 pos = m_seekPreview >= 0 ? m_seekPreview : m_plugin->mpris()->positionUs();
    return formatTime(pos, st.track.lengthUs >= 3600LL * 1'000'000);
}

QString MediaPane::totalText() const
{
    if (!m_plugin) return {};
    const qint64 len = m_plugin->mpris()->state().track.lengthUs;
    return len > 0 ? formatTime(len, len >= 3600LL * 1'000'000) : QStringLiteral("--:--");
}

void MediaPane::refresh()
{
    if (!m_plugin) return;
    const PlayerState &st = m_plugin->mpris()->state();
    const bool session = !st.service.isEmpty();
    const bool anim = isVisible();
    m_play->setSource(icon(st.status == QLatin1String("Playing") ? QStringLiteral("pause") : QStringLiteral("play")), anim);
    m_shuffle->setSource(icon(st.shuffle ? QStringLiteral("shuffle-on") : QStringLiteral("shuffle-off")), anim);
    m_loop->setSource(icon(st.loop == QLatin1String("Track") ? QStringLiteral("loop-track")
                           : st.loop == QLatin1String("Playlist") ? QStringLiteral("loop-playlist")
                                                                   : QStringLiteral("loop-none")),
                      anim);
    const shade::Theme &t = shade::Theme::get();
    m_shuffle->setColor(st.shuffle ? t.tx() : t.tx3());
    m_loop->setColor(st.loop != QLatin1String("None") ? t.tx() : t.tx3());
    m_shuffle->setEnabled(st.hasShuffle && st.canControl);
    m_loop->setEnabled(st.hasLoop && st.canControl);
    m_prev->setEnabled(st.canPrevious);
    m_next->setEnabled(st.canNext);
    m_play->setEnabled(st.canPlay || st.canPause);
    for (shade::IconButton *b : {m_shuffle, m_prev, m_play, m_next, m_loop}) b->setVisible(session);
    if (isVisible() && st.status == QLatin1String("Playing")) m_progress->start();
    else m_progress->stop();
    if (size() != sizeHint()) notifySizeChanged();
    layoutButtons();
    update();
}

void MediaPane::setArt(const QImage &art)
{
    m_artPrev = m_art;
    m_bgPrev = m_bg;
    m_art = art;
    m_bg = blurredBackground(art, QSize(kWidth, kHeight));
    m_artFade = m_artPrev.isNull() && art.isNull() ? 1.0 : 0.0;
    if (m_artFade < 1.0) m_fade->start();
    update();
    requestPeekRepaint();
}

void MediaPane::layoutButtons()
{
    // Controls row under the text, centered in the area right of the art.
    const int y = 60;
    const QList<shade::IconButton *> row{m_shuffle, m_prev, m_play, m_next, m_loop};
    int total = 0;
    for (auto *b : row) total += b->sizeHint().width();
    const int spacing = 6;
    total += spacing * (row.size() - 1);
    const int left = 92, right = width() - 14;
    int x = left + std::max(0, (right - left - total) / 2);
    for (auto *b : row) {
        const QSize s = b->sizeHint();
        b->setGeometry(x, y + (32 - s.height()) / 2, s.width(), s.height());
        x += s.width() + spacing;
    }
}

void MediaPane::resizeEvent(QResizeEvent *) { layoutButtons(); }

void MediaPane::showEvent(QShowEvent *)
{
    refresh();
}

void MediaPane::hideEvent(QHideEvent *) { m_progress->stop(); }

void MediaPane::paintArt(QPainter *p, const QRectF &r, qreal radius, qreal fade) const
{
    const shade::Theme &t = shade::Theme::get();
    QPainterPath clip;
    clip.addRoundedRect(r, radius, radius);
    p->save();
    p->setClipPath(clip, Qt::IntersectClip);
    auto draw = [&](const QImage &img, qreal opacity) {
        if (opacity <= 0.001) return;
        p->setOpacity(p->opacity() * opacity);
        if (img.isNull()) {
            p->fillRect(r, t.ui());
            const qreal s = r.width() * 0.42;
            m_note->paint(p, QRectF(r.center().x() - s / 2, r.center().y() - s / 2, s, s), t.tx3());
        } else {
            const QSizeF cover = QSizeF(img.size()).scaled(r.size(), Qt::KeepAspectRatioByExpanding);
            p->drawImage(QRectF(r.center().x() - cover.width() / 2, r.center().y() - cover.height() / 2, cover.width(), cover.height()), img);
        }
    };
    const qreal base = p->opacity();
    draw(m_artPrev, 1.0);
    p->setOpacity(base);
    draw(m_art, fade);
    p->restore();
}

void MediaPane::paintEvent(QPaintEvent *)
{
    const shade::Theme &t = shade::Theme::get();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF r = rect();
    const QPainterPath rounded = roundedRect();

    if (!m_plugin || !m_plugin->mpris()->hasSession()) {
        m_note->paint(&p, QRectF(18, (height() - 16) / 2.0, 16, 16), t.tx3());
        p.setFont(shade::Theme::font(13, QFont::Medium));
        p.setPen(t.tx2());
        p.drawText(r.adjusted(44, 0, -14, 0), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Nothing playing"));
        return;
    }

    // Background: blurred art, dimmed, inside the pill's rounded shape (the
    // live widget is not clipped by the window).
    p.save();
    p.setClipPath(rounded);
    p.fillRect(r, t.bg());
    if (!m_bgPrev.isNull()) {
        p.setOpacity(0.55);
        p.drawImage(r, m_bgPrev);
    }
    if (!m_bg.isNull()) {
        p.setOpacity(0.55 * m_artFade);
        p.drawImage(r, m_bg);
    }
    p.setOpacity(1.0);
    QLinearGradient shadeGrad(r.topLeft(), r.bottomLeft());
    shadeGrad.setColorAt(0, shade::withAlpha(t.bg(), 0.35));
    shadeGrad.setColorAt(1, shade::withAlpha(t.bg(), 0.7));
    p.fillRect(r, shadeGrad);
    p.restore();

    paintArt(&p, artRect(), 10, m_artFade);

    const PlayerState &st = m_plugin->mpris()->state();
    const qreal tx = 92, tw = width() - tx - 16;
    p.setPen(t.tx());
    p.setFont(shade::Theme::font(14, QFont::DemiBold));
    const QString title = st.track.title.isEmpty() ? st.identity : st.track.title;
    p.drawText(QRectF(tx, 14, tw, 20), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(p.font()).elidedText(title, Qt::ElideRight, int(tw)));
    p.setPen(t.tx2());
    p.setFont(shade::Theme::font(12));
    QString sub = st.track.artist;
    if (!st.track.album.isEmpty()) sub += (sub.isEmpty() ? QString() : QStringLiteral(" · ")) + st.track.album;
    if (sub.isEmpty()) sub = st.identity;
    p.drawText(QRectF(tx, 35, tw, 18), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(p.font()).elidedText(sub, Qt::ElideRight, int(tw)));

    // Seek bar with fixed-width time labels on both sides.
    const QRectF bar = seekRect();
    const QFontMetrics fm(timeFont());
    const qreal lw = fm.horizontalAdvance(QStringLiteral("0:00:00")) + 2;
    p.setFont(timeFont());
    p.setPen(t.tx2());
    p.drawText(QRectF(92, bar.center().y() - 9, lw, 18), Qt::AlignRight | Qt::AlignVCenter, elapsedText());
    p.drawText(QRectF(bar.right() + 8, bar.center().y() - 9, lw, 18), Qt::AlignLeft | Qt::AlignVCenter, totalText());
    p.setPen(Qt::NoPen);
    p.setBrush(shade::withAlpha(t.tx(), 0.18));
    p.drawRoundedRect(bar, 2, 2);
    const qint64 len = st.track.lengthUs;
    if (len > 0) {
        const qint64 pos = m_seekPreview >= 0 ? m_seekPreview : m_plugin->mpris()->positionUs();
        const qreal frac = std::clamp(double(pos) / double(len), 0.0, 1.0);
        QRectF fill = bar;
        fill.setWidth(bar.width() * frac);
        p.setBrush(t.tx());
        if (fill.width() > 0.5) p.drawRoundedRect(fill, 2, 2);
        if (st.canSeek && (m_seeking || bar.adjusted(-4, -8, 4, 8).contains(mapFromGlobal(QCursor::pos())))) {
            p.drawEllipse(QPointF(fill.right(), bar.center().y()), 5, 5);
        }
    }
}

qint64 MediaPane::seekAt(qreal x) const
{
    const QRectF bar = seekRect();
    const qreal frac = std::clamp((x - bar.left()) / bar.width(), 0.0, 1.0);
    return qint64(frac * double(m_plugin->mpris()->state().track.lengthUs));
}

void MediaPane::mousePressEvent(QMouseEvent *e)
{
    if (m_plugin && e->button() == Qt::LeftButton && m_plugin->mpris()->state().canSeek &&
        seekRect().adjusted(-4, -8, 4, 8).contains(e->position())) {
        m_seeking = true;
        m_seekPreview = seekAt(e->position().x());
        update();
        e->accept();
        return;
    }
    e->ignore();
}

void MediaPane::mouseMoveEvent(QMouseEvent *e)
{
    if (m_seeking) m_seekPreview = seekAt(e->position().x());
    update();
}

void MediaPane::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_seeking) {
        e->ignore();
        return;
    }
    m_seeking = false;
    m_plugin->mpris()->seekTo(seekAt(e->position().x()));
    m_seekPreview = -1;
    update();
}

// --- peek -----------------------------------------------------------------

QSize MediaPane::peekSizeHint(const shade::PeekView &peek) const
{
    const QString title = peek.data.value(QStringLiteral("title")).toString();
    const QString artist = peek.data.value(QStringLiteral("artist")).toString();
    const int tw = std::max(QFontMetrics(shade::Theme::font(13, QFont::DemiBold)).horizontalAdvance(title),
                            QFontMetrics(shade::Theme::font(11)).horizontalAdvance(artist));
    return QSize(std::clamp(8 + 30 + 10 + tw + 18, 160, 360), 46);
}

void MediaPane::paintPeek(QPainter *p, const QRectF &rect, const shade::PeekView &peek, qreal morph)
{
    const shade::Theme &t = shade::Theme::get();
    const QString title = peek.data.value(QStringLiteral("title")).toString();
    const QString artist = peek.data.value(QStringLiteral("artist")).toString();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    // Shared element: the thumbnail travels and grows towards its place in
    // the full widget as the peek morphs into it.
    const QRectF small(rect.left() + 8, rect.center().y() - 15, 30, 30);
    const QRectF big = artRect().translated(rect.topLeft());
    const qreal m = std::clamp(morph, 0.0, 1.0);
    const QRectF art(shade::lerp(small.left(), big.left(), m), shade::lerp(small.top(), big.top(), m),
                     shade::lerp(small.width(), big.width(), m), shade::lerp(small.height(), big.height(), m));
    paintArt(p, art, shade::lerp(8.0, 10.0, m), m_artFade);

    const qreal x = small.right() + 10;
    const qreal w = rect.right() - 14 - x;
    p->setPen(t.tx());
    p->setFont(shade::Theme::font(13, QFont::DemiBold));
    p->drawText(QRectF(x, rect.center().y() - 17, w, 18), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(p->font()).elidedText(title, Qt::ElideRight, int(w)));
    p->setPen(t.tx2());
    p->setFont(shade::Theme::font(11));
    p->drawText(QRectF(x, rect.center().y() + 1, w, 16), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(p->font()).elidedText(artist, Qt::ElideRight, int(w)));
    p->restore();
}

} // namespace media
