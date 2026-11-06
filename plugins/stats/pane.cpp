#include "pane.h"

#include "plugin.h"

#include <shade/theme.h>

#include <QDateTime>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>

namespace stats
{

namespace
{
constexpr int kIcon = 16;
constexpr int kPadX = 7;
constexpr int kHeight = 30;

QFont valueFont() { return shade::Theme::font(12, QFont::DemiBold); }
QFont detailFont() { return shade::Theme::font(12); }
} // namespace

// ---------------------------------------------------------------------------
// StatItem
// ---------------------------------------------------------------------------

StatItem::StatItem(const QString &name, QWidget *parent)
    : QWidget(parent)
    , m_icon(new shade::MorphIcon(this))
{
    setObjectName(name);
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_icon->setDuration(380);
    connect(m_icon, &shade::MorphIcon::changed, this, qOverload<>(&QWidget::update));
    m_anim = new QTimer(this);
    m_anim->setInterval(16);
    m_anim->setTimerType(Qt::PreciseTimer);
    connect(m_anim, &QTimer::timeout, this, &StatItem::animate);
}

void StatItem::setIcon(const QString &source) { m_icon->setSource(source, isVisible()); }

void StatItem::setValue(const QString &value)
{
    if (value == m_value) return;
    m_value = value;
    updateGeometry();
    update();
}

void StatItem::setDetail(const QString &detail)
{
    if (detail == m_detail) return;
    m_detail = detail;
    if (m_reveal > 0) updateGeometry();
    update();
}

void StatItem::setDimmed(bool dimmed)
{
    if (dimmed == m_dimmed) return;
    m_dimmed = dimmed;
    update();
}

void StatItem::setInteractive(bool interactive)
{
    m_interactive = interactive;
    setCursor(interactive ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

QSize StatItem::sizeHint() const
{
    int w = kPadX * 2 + kIcon;
    if (!m_value.isEmpty()) w += 5 + QFontMetrics(valueFont()).horizontalAdvance(m_value);
    if (!m_detail.isEmpty() && m_reveal > 0.001) {
        const int full = 6 + std::min(180, QFontMetrics(detailFont()).horizontalAdvance(m_detail));
        w += int(std::ceil(full * std::clamp(m_reveal, 0.0, 1.2)));
    }
    return QSize(w, kHeight);
}

void StatItem::animate()
{
    // Hover highlight: simple ease; detail reveal: a slightly bouncy spring.
    const qreal hoverTarget = m_hovered ? 1.0 : 0.0;
    m_hover += (hoverTarget - m_hover) * 0.25;
    const qreal revealTarget = m_hovered && !m_detail.isEmpty() ? 1.0 : 0.0;
    const qreal k = 420, c = 2 * 0.78 * std::sqrt(k), dt = 0.016;
    m_revealVel += (-k * (m_reveal - revealTarget) - c * m_revealVel) * dt;
    m_reveal += m_revealVel * dt;
    if (std::abs(m_reveal - revealTarget) < 0.002 && std::abs(m_revealVel) < 0.02 && std::abs(m_hover - hoverTarget) < 0.01) {
        m_reveal = revealTarget;
        m_revealVel = 0;
        m_hover = hoverTarget;
        m_anim->stop();
    }
    updateGeometry();
    update();
}

void StatItem::paintEvent(QPaintEvent *)
{
    const shade::Theme &t = shade::Theme::get();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    if (m_hover > 0.01 && m_interactive) {
        p.setPen(Qt::NoPen);
        p.setBrush(shade::withAlpha(t.ui2(), m_hover));
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    }
    const QColor fg = m_dimmed ? t.tx3() : t.tx();
    qreal x = kPadX;
    m_icon->paint(&p, QRectF(x, (height() - kIcon) / 2.0, kIcon, kIcon), fg);
    x += kIcon;
    if (!m_value.isEmpty()) {
        x += 5;
        p.setFont(valueFont());
        p.setPen(fg);
        const int w = QFontMetrics(valueFont()).horizontalAdvance(m_value);
        p.drawText(QRectF(x, 0, w + 2, height()), Qt::AlignVCenter | Qt::AlignLeft, m_value);
        x += w;
    }
    if (!m_detail.isEmpty() && m_reveal > 0.001) {
        x += 6;
        p.setFont(detailFont());
        const QFontMetrics fm(detailFont());
        const QString text = fm.elidedText(m_detail, Qt::ElideRight, 180);
        const qreal avail = std::max<qreal>(0, width() - kPadX - x);
        p.save();
        p.setClipRect(QRectF(x, 0, avail, height()));
        p.setPen(shade::withAlpha(t.tx2(), std::clamp(m_reveal, 0.0, 1.0)));
        p.drawText(QRectF(x - (1 - std::min<qreal>(1, m_reveal)) * 8, 0, 200, height()), Qt::AlignVCenter | Qt::AlignLeft, text);
        p.restore();
    }
}

void StatItem::enterEvent(QEnterEvent *)
{
    m_hovered = true;
    m_anim->start();
}

void StatItem::leaveEvent(QEvent *)
{
    m_hovered = false;
    m_anim->start();
}

void StatItem::mousePressEvent(QMouseEvent *e) { e->accept(); }

void StatItem::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_interactive && rect().contains(e->position().toPoint())) emit clicked(e->button());
    e->accept();
}

void StatItem::wheelEvent(QWheelEvent *e)
{
    // Vertical only; horizontal wheel pages the shade.
    const QPoint px = e->pixelDelta(), ang = e->angleDelta();
    const qreal dy = px.isNull() ? ang.y() / 120.0 * 40.0 : px.y();
    const qreal dx = px.isNull() ? ang.x() / 120.0 * 40.0 : px.x();
    if (std::abs(dx) > std::abs(dy) || !receivers(SIGNAL(scrolled(int)))) {
        e->ignore();
        return;
    }
    m_wheelAccum += dy;
    const int steps = int(m_wheelAccum / 30.0);
    if (steps != 0) {
        m_wheelAccum -= steps * 30.0;
        emit scrolled(steps);
    }
    e->accept();
}

// ---------------------------------------------------------------------------
// StatsPane
// ---------------------------------------------------------------------------

StatsPane::StatsPane(StatsPlugin *plugin, const QString &window, QWidget *parent)
    : shade::PaneWidget(parent)
    , m_plugin(plugin)
    , m_window(window)
{
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(14, 7, 10, 7);
    row->setSpacing(2);
    row->setSizeConstraint(QLayout::SetFixedSize);

    m_time = new QLabel(this);
    m_time->setFont(shade::Theme::font(13, QFont::DemiBold));
    QPalette pal = m_time->palette();
    pal.setColor(QPalette::WindowText, shade::Theme::get().tx());
    m_time->setPalette(pal);
    // Fixed width: digits change every second, the layout must not move.
    m_time->setFixedWidth(QFontMetrics(m_time->font()).horizontalAdvance(QStringLiteral("88:88:88")) + 6);
    row->addWidget(m_time);
    row->addSpacing(4);

    for (const QString &name : plugin->order()) {
        if (name == QLatin1String("time")) continue;
        auto *item = new StatItem(name, this);
        m_items.insert(name, item);
        row->addWidget(item);
        if (name == QLatin1String("network")) {
            connect(item, &StatItem::clicked, plugin, [plugin, window](Qt::MouseButton b) {
                if (b == Qt::LeftButton) plugin->launch(QStringLiteral("network"), window);
            });
        } else if (name == QLatin1String("bluetooth")) {
            connect(item, &StatItem::clicked, plugin, [plugin, window](Qt::MouseButton b) {
                if (b == Qt::LeftButton) plugin->launch(QStringLiteral("bluetooth"), window);
            });
        } else if (name == QLatin1String("volume")) {
            connect(item, &StatItem::clicked, plugin, [plugin, window](Qt::MouseButton b) {
                if (b == Qt::LeftButton) plugin->launch(QStringLiteral("volume"), window);
                else if (b == Qt::MiddleButton) plugin->volume()->toggleMute();
            });
            connect(item, &StatItem::scrolled, plugin, [plugin](int steps) { plugin->volume()->adjust(steps); });
        } else {
            item->setInteractive(false);
        }
    }
    if (!plugin->order().contains(QStringLiteral("time"))) m_time->hide();

    connect(plugin, &StatsPlugin::stateChanged, this, &StatsPane::refresh);
    connect(plugin, &StatsPlugin::timeTick, this, &StatsPane::tick);

    m_levelAnim = new QTimer(this);
    m_levelAnim->setInterval(16);
    connect(m_levelAnim, &QTimer::timeout, this, [this]() {
        bool moving = false;
        for (auto it = m_peekLevels.begin(); it != m_peekLevels.end(); ++it) {
            const qreal target = m_peekTargets.value(it.key());
            *it += (target - *it) * 0.22;
            if (std::abs(target - *it) < 0.002) *it = target;
            else moving = true;
        }
        requestPeekRepaint();
        if (!moving) m_levelAnim->stop();
    });

    tick();
    refresh();
}

QSize StatsPane::sizeHint() const
{
    return layout()->sizeHint().expandedTo(QSize(120, 44));
}

StatItem *StatsPane::item(const QString &name) const { return m_items.value(name); }

QString StatsPane::timeText() const { return m_time->text(); }

void StatsPane::tick()
{
    const QString text = QDateTime::currentDateTime().toString(m_plugin ? m_plugin->timeFormat() : QStringLiteral("HH:mm:ss"));
    if (text != m_time->text()) m_time->setText(text);
}

void StatsPane::refresh()
{
    if (!m_plugin) return;
    if (StatItem *it = m_items.value(QStringLiteral("network"))) {
        Network *n = m_plugin->network();
        it->setIcon(n->icon());
        it->setDetail(n->describe());
        it->setDimmed(n->kind() == Network::Kind::Offline);
    }
    if (StatItem *it = m_items.value(QStringLiteral("bluetooth"))) {
        Bluetooth *b = m_plugin->bluetooth();
        it->setIcon(b->icon());
        it->setDetail(b->describe());
        it->setDimmed(b->state() == Bluetooth::State::Off || b->state() == Bluetooth::State::Unavailable);
    }
    if (StatItem *it = m_items.value(QStringLiteral("battery"))) {
        Battery *b = m_plugin->battery();
        it->setVisible(b->present() || !m_plugin->hideMissingBattery());
        it->setIcon(b->icon());
        it->setValue(b->present() ? QStringLiteral("%1%").arg(std::lround(b->percentage())) : QString());
        it->setDetail(b->present() ? b->describe().section(QStringLiteral(" · "), 0, 0) : QStringLiteral("No battery"));
        it->setDimmed(!b->present());
    }
    if (StatItem *it = m_items.value(QStringLiteral("volume"))) {
        Volume *v = m_plugin->volume();
        it->setIcon(v->icon());
        it->setValue(v->available() ? QStringLiteral("%1%").arg(v->level()) : QString());
        it->setDetail(v->available() ? (v->muted() ? QStringLiteral("Muted") : QString()) : QStringLiteral("No audio"));
        it->setDimmed(!v->available() || v->muted());
    }
}

shade::MorphIcon *StatsPane::peekIcon(const QString &id, const QString &source)
{
    shade::MorphIcon *icon = m_peekIcons.value(id);
    if (!icon) {
        icon = new shade::MorphIcon(this);
        icon->setDuration(420);
        icon->setSource(source, false);
        connect(icon, &shade::MorphIcon::changed, this, &shade::PaneWidget::requestPeekRepaint);
        m_peekIcons.insert(id, icon);
    } else if (icon->source() != source) {
        icon->setSource(source, true);
    }
    return icon;
}

qreal StatsPane::peekLevel(const QString &id, qreal target)
{
    if (!m_peekLevels.contains(id)) m_peekLevels.insert(id, target);
    if (!qFuzzyCompare(m_peekTargets.value(id, -1) + 1, target + 1)) {
        m_peekTargets.insert(id, target);
        m_levelAnim->start();
    }
    return m_peekLevels.value(id);
}

QSize StatsPane::peekSizeHint(const shade::PeekView &peek) const
{
    const QString text = peek.data.value(QStringLiteral("text")).toString();
    const int value = peek.data.value(QStringLiteral("value"), -1).toInt();
    const bool bar = peek.id == QLatin1String("volume") && value >= 0;
    int w = 16 + 18 + 9 + QFontMetrics(shade::Theme::font(13, QFont::DemiBold)).horizontalAdvance(text) + 18;
    if (bar) w += 90 + 10;
    return QSize(std::clamp(w, 120, 420), 36);
}

void StatsPane::paintPeek(QPainter *p, const QRectF &rect, const shade::PeekView &peek, qreal)
{
    const shade::Theme &t = shade::Theme::get();
    const QString text = peek.data.value(QStringLiteral("text")).toString();
    const QString icon = peek.data.value(QStringLiteral("icon")).toString();
    const int value = peek.data.value(QStringLiteral("value"), -1).toInt();
    const bool bar = peek.id == QLatin1String("volume") && value >= 0;
    const QSize natural = peekSizeHint(peek);

    // Lay out on the natural size, centered in the (possibly animating) rect.
    QRectF r(QPointF(0, 0), QSizeF(natural));
    r.moveCenter(rect.center());
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    qreal x = r.left() + 16;
    peekIcon(peek.id, icon)->paint(p, QRectF(x, r.center().y() - 9, 18, 18), t.tx());
    x += 18 + 9;
    p->setFont(shade::Theme::font(13, QFont::DemiBold));
    p->setPen(t.tx());
    const int tw = QFontMetrics(p->font()).horizontalAdvance(text);
    p->drawText(QRectF(x, r.top(), tw + 4, r.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
    x += tw + 10;
    if (bar) {
        const QRectF track(x, r.center().y() - 2.5, 90, 5);
        p->setPen(Qt::NoPen);
        p->setBrush(shade::withAlpha(t.tx(), 0.18));
        p->drawRoundedRect(track, 2.5, 2.5);
        const qreal level = peekLevel(peek.id, std::clamp(value / 100.0, 0.0, 1.0));
        QRectF fill = track;
        fill.setWidth(std::max(track.height(), track.width() * level));
        if (level > 0.001) {
            p->setBrush(t.tx());
            p->drawRoundedRect(fill, 2.5, 2.5);
        }
    }
    p->restore();
}

} // namespace stats
