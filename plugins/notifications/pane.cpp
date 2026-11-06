#include "pane.h"

#include "model.h"
#include "plugin.h"
#include "server.h"

#include <shade/theme.h>

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QTimer>

#include <cmath>

namespace notif
{

namespace
{
constexpr qreal kPad = 10;     // pane padding
constexpr qreal kCardPad = 10; // card padding
constexpr qreal kGap = 6;
constexpr qreal kFooter = 26;
constexpr qreal kPlaceholder = 40;
constexpr int kStaggerMs = 50;

QFont sourceFont() { return shade::Theme::font(11, QFont::Medium); }
QFont titleFont() { return shade::Theme::font(13, QFont::DemiBold); }
QFont bodyFont() { return shade::Theme::font(12); }

// Wraps at words, breaking long tokens anywhere (a URL must not widen the
// pane). Returns the height; paints when `p` is set.
qreal drawWrapped(QPainter *p, const QString &text, const QFont &font, const QRectF &r, int maxLines = 0)
{
    if (text.isEmpty()) return 0;
    QTextLayout layout(text, font);
    QTextOption opt;
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(opt);
    layout.beginLayout();
    qreal y = 0;
    int lines = 0;
    const QFontMetricsF fm(font);
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(r.width());
        if (maxLines > 0 && lines == maxLines - 1) {
            // Last allowed line: elide the remainder.
            const QString rest = text.mid(line.textStart());
            line.setLineWidth(1e6);
            line.setPosition(QPointF(0, y));
            layout.endLayout();
            if (p) {
                for (int i = 0; i < lines; ++i) layout.lineAt(i).draw(p, r.topLeft());
                p->setFont(font);
                p->drawText(QPointF(r.left(), r.top() + y + fm.ascent()), fm.elidedText(rest.simplified(), Qt::ElideRight, r.width()));
            }
            return y + fm.height();
        }
        line.setPosition(QPointF(0, y));
        y += line.height();
        ++lines;
    }
    layout.endLayout();
    if (p) layout.draw(p, r.topLeft());
    return y;
}

} // namespace

NotificationsPane::NotificationsPane(NotificationsPlugin *plugin, QWidget *parent)
    : shade::PaneWidget(parent)
    , m_plugin(plugin)
{
    setMouseTracking(true);
    m_clock.start();
    m_tick = new QTimer(this);
    m_tick->setInterval(16);
    m_tick->setTimerType(Qt::PreciseTimer);
    connect(m_tick, &QTimer::timeout, this, &NotificationsPane::step);
    m_ageTimer = new QTimer(this);
    m_ageTimer->setInterval(30000);
    connect(m_ageTimer, &QTimer::timeout, this, qOverload<>(&QWidget::update));

    Model *model = plugin->model();
    connect(model, &Model::added, this, &NotificationsPane::onAdded);
    connect(model, &Model::replaced, this, [this]() { relayout(); });
    connect(model, &Model::removed, this, &NotificationsPane::onRemoved);
    connect(model, &Model::cleared, this, &NotificationsPane::onCleared);
    connect(model, &Model::dndChanged, this, [this](bool on) {
        if (on) onCleared(); // sweep the rows away (they stay stored)
        else rebuild(true);
    });
    rebuild(false);
    m_height = targetHeight();
    resize(kWidth, int(std::ceil(m_height)));
}

QString NotificationsPane::placeholderLink() const
{
    return m_plugin && m_plugin->model()->dnd() ? QStringLiteral("unmute.") : QStringLiteral("keep it this way.");
}

QSize NotificationsPane::sizeHint() const { return QSize(kWidth, std::max(24, int(std::ceil(m_height)))); }

qreal NotificationsPane::cardHeight(const Notification &n, qreal width) const
{
    const qreal inner = width - 2 * kCardPad;
    qreal h = kCardPad + QFontMetricsF(sourceFont()).height() + 2;
    h += drawWrapped(nullptr, n.summary, titleFont(), QRectF(0, 0, inner, 0), 3);
    const QString body = Model::truncateBody(n.body);
    if (!body.isEmpty()) h += 2 + drawWrapped(nullptr, body, bodyFont(), QRectF(0, 0, inner, 0));
    return std::ceil(h + kCardPad);
}

void NotificationsPane::rebuild(bool animate)
{
    m_rows.clear();
    if (!m_plugin || m_plugin->model()->dnd()) {
        relayout();
        return;
    }
    for (const Notification &n : m_plugin->model()->items()) {
        Row r;
        r.id = n.id;
        r.opacity = animate ? 0 : 1;
        m_rows << r;
    }
    relayout();
    if (!animate) {
        for (Row &r : m_rows) {
            r.y = r.targetY;
            r.opacity = r.targetOpacity;
        }
        m_placeholder = m_rows.isEmpty() ? 1 : 0;
    }
}

void NotificationsPane::relayout(qint64 staggerFrom, int staggerIndex)
{
    // Targets from the model order; dying rows keep their place.
    const qreal w = kWidth - 2 * kPad;
    qreal y = kPad;
    int visible = 0;
    int moved = 0;
    for (Row &r : m_rows) {
        if (r.dying) continue;
        const Notification *n = m_plugin ? m_plugin->model()->find(r.id) : nullptr;
        if (!n) continue;
        m_ghosts.insert(r.id, *n);
        r.height = cardHeight(*n, w);
        r.hidden = visible >= kMaxRows;
        const qreal target = r.hidden ? y + 8 : y;
        if (staggerFrom >= 0 && std::abs(target - r.targetY) > 0.5 && visible >= staggerIndex) {
            r.startAt = staggerFrom + kStaggerMs * (++moved);
        }
        if (r.hidden && r.opacity < 0.01) r.y = target; // hidden rows wait in place
        r.targetY = target;
        r.targetOpacity = r.hidden ? 0 : 1;
        if (!r.hidden) {
            y += r.height + kGap;
            ++visible;
        }
    }
    ensureTicking();
}

void NotificationsPane::onAdded(quint32 id)
{
    if (!m_plugin || m_plugin->model()->dnd()) return;
    Row r;
    r.id = id;
    r.opacity = 0;
    r.y = kPad - 12; // drops in from above while the stack slides down
    m_rows.prepend(r);
    relayout();
}

void NotificationsPane::onRemoved(quint32 id)
{
    // Staggered wave: the row fades in place; the rows below follow one
    // after another, 50 ms apart.
    int index = 0;
    for (Row &r : m_rows) {
        if (r.id == id && !r.dying) {
            r.dying = true;
            r.targetOpacity = 0;
            break;
        }
        if (!r.dying && !r.hidden) ++index;
    }
    relayout(m_clock.elapsed() + 60, index);
}

void NotificationsPane::onCleared()
{
    // Sweep: everything fades together while the stack glides down.
    for (Row &r : m_rows) {
        r.dying = true;
        r.targetOpacity = 0;
        r.targetY = r.y + 14;
        r.startAt = 0;
    }
    relayout();
}

qreal NotificationsPane::contentHeight() const
{
    // Animated: the bottom of the lowest live (or still visible) row.
    qreal bottom = 0;
    for (const Row &r : m_rows) {
        if (r.hidden && r.opacity < 0.01) continue;
        const qreal weight = r.dying ? r.opacity : 1.0;
        bottom = std::max(bottom, r.y + r.height * weight);
    }
    bottom = bottom > 0 ? bottom + kPad : 0;
    const qreal placeholder = kPlaceholder * m_placeholder;
    return std::max(bottom + kFooter * m_footer, placeholder);
}

int NotificationsPane::targetHeight() const
{
    qreal bottom = 0;
    int live = 0;
    for (const Row &r : m_rows) {
        if (r.dying || r.hidden) continue;
        bottom = std::max(bottom, r.targetY + r.height);
        ++live;
    }
    const int total = m_plugin && !m_plugin->model()->dnd() ? int(m_plugin->model()->items().size()) : 0;
    if (live == 0) return int(kPlaceholder);
    return int(std::ceil(bottom + kPad + (total > kMaxRows ? kFooter : 0)));
}

bool NotificationsPane::animating() const { return m_tick->isActive(); }

void NotificationsPane::ensureTicking()
{
    if (!m_tick->isActive()) {
        m_last = m_clock.elapsed();
        m_tick->start();
    }
}

void NotificationsPane::step()
{
    const qint64 now = m_clock.elapsed();
    const qreal dt = std::clamp((now - m_last) / 1000.0, 0.001, 0.05);
    m_last = now;
    bool moving = false;
    const qreal k = 460, c = 2 * 0.86 * std::sqrt(k);
    for (Row &r : m_rows) {
        if (now >= r.startAt) {
            // Substeps keep the spring stable on slow frames.
            for (qreal t = 0; t < dt; t += 1.0 / 240) {
                const qreal h = std::min(1.0 / 240, dt - t);
                r.vy += (-k * (r.y - r.targetY) - c * r.vy) * h;
                r.y += r.vy * h;
            }
        }
        const qreal fadeRate = r.dying ? 1.0 / 0.18 : 1.0 / 0.22;
        if (r.opacity < r.targetOpacity) r.opacity = std::min(r.targetOpacity, r.opacity + dt * fadeRate);
        else if (r.opacity > r.targetOpacity) r.opacity = std::max(r.targetOpacity, r.opacity - dt * fadeRate);
        const bool hoverIn = !r.dying && rowRect(r.id).contains(m_mouse);
        r.hover += ((hoverIn ? 1.0 : 0.0) - r.hover) * std::min(1.0, dt * 14);
        if (std::abs(r.y - r.targetY) > 0.2 || std::abs(r.vy) > 1 || r.opacity != r.targetOpacity ||
            std::abs(r.hover - (hoverIn ? 1 : 0)) > 0.01) {
            moving = true;
        } else {
            r.y = r.targetY;
            r.vy = 0;
        }
    }
    // Dead rows go once faded; the survivors' targets already exclude them.
    m_rows.removeIf([this](const Row &r) {
        const bool gone = r.dying && r.opacity <= 0.0;
        if (gone && !(m_plugin && m_plugin->model()->find(r.id))) m_ghosts.remove(r.id);
        return gone;
    });

    const bool empty = std::none_of(m_rows.cbegin(), m_rows.cend(), [](const Row &r) { return !r.dying; });
    const qreal phTarget = empty ? 1 : 0;
    m_placeholder += (phTarget - m_placeholder) * std::min(1.0, dt * 12);
    if (std::abs(m_placeholder - phTarget) < 0.005) m_placeholder = phTarget;
    else moving = true;
    const int total = m_plugin && !m_plugin->model()->dnd() ? int(m_plugin->model()->items().size()) : 0;
    const qreal ftTarget = total > kMaxRows ? 1 : 0;
    m_footer += (ftTarget - m_footer) * std::min(1.0, dt * 12);
    if (std::abs(m_footer - ftTarget) < 0.005) m_footer = ftTarget;
    else moving = true;

    // Height follows the content (critically damped, no overshoot past it).
    const qreal want = std::max<qreal>(24, contentHeight());
    m_height = want;
    const int h = int(std::ceil(m_height));
    if (h != m_lastReported) {
        m_lastReported = h;
        notifySizeChanged();
    }
    if (!moving) m_tick->stop();
    update();
    requestPeekRepaint();
}

QList<quint32> NotificationsPane::visibleIds() const
{
    QList<quint32> out;
    for (const Row &r : m_rows) {
        if (!r.dying && !r.hidden) out << r.id;
    }
    return out;
}

QRectF NotificationsPane::rowRect(quint32 id) const
{
    for (const Row &r : m_rows) {
        if (r.id == id) return QRectF(kPad, r.y, kWidth - 2 * kPad, r.height);
    }
    return {};
}

QRectF NotificationsPane::clearAllRect() const
{
    if (m_footer < 0.5) return {};
    const qreal y = height() - kFooter;
    return QRectF(kPad + 4, y, QFontMetricsF(bodyFont()).horizontalAdvance(QStringLiteral("clear all")) + 4, kFooter - 6);
}

QRectF NotificationsPane::dndLinkRect() const
{
    if (m_placeholder < 0.5 || !m_plugin || !m_plugin->daemonRunning()) return {};
    const QFontMetricsF fm(bodyFont());
    const bool dnd = m_plugin && m_plugin->model()->dnd();
    const QString lead = dnd ? QString() : QStringLiteral("quiet. ");
    const qreal total = fm.horizontalAdvance(lead) + fm.horizontalAdvance(placeholderLink());
    const qreal x = (width() - total) / 2 + fm.horizontalAdvance(lead);
    return QRectF(x - 2, (kPlaceholder - 20) / 2, fm.horizontalAdvance(placeholderLink()) + 4, 20);
}

void NotificationsPane::paintCard(QPainter *p, const QRectF &r, const Notification &n, qreal hover, bool compact) const
{
    const shade::Theme &t = shade::Theme::get();
    const bool critical = n.urgency == Critical;
    const QColor bg = critical ? t.tx() : shade::mix(t.ui(), t.ui2(), hover);
    const QColor fg = critical ? t.bg() : t.tx();
    const QColor fg2 = critical ? shade::withAlpha(t.bg(), 0.7) : t.tx2();
    const qreal radius = compact ? std::max<qreal>(6, cornerRadius() - 6) : std::max<qreal>(6, cornerRadius() - kPad);
    p->setPen(Qt::NoPen);
    p->setBrush(bg);
    p->drawRoundedRect(r, radius, radius);

    const QRectF inner = r.adjusted(kCardPad, kCardPad, -kCardPad, -kCardPad);
    const QFontMetricsF sf(sourceFont());
    p->setFont(sourceFont());
    p->setPen(fg2);
    const QString age = Model::age(n.received);
    const qreal ageW = sf.horizontalAdvance(age);
    const QString app = n.app.isEmpty() ? QStringLiteral("notification") : n.app;
    p->drawText(QRectF(inner.left(), inner.top(), inner.width() - ageW - 8, sf.height()), Qt::AlignLeft | Qt::AlignVCenter,
                sf.elidedText(app, Qt::ElideRight, inner.width() - ageW - 8));
    p->drawText(QRectF(inner.right() - ageW, inner.top(), ageW, sf.height()), Qt::AlignRight | Qt::AlignVCenter, age);
    qreal y = inner.top() + sf.height() + 2;
    p->setPen(fg);
    y += drawWrapped(p, n.summary, titleFont(), QRectF(inner.left(), y, inner.width(), 0), compact ? 1 : 3);
    const QString body = Model::truncateBody(n.body);
    if (!body.isEmpty()) {
        p->setPen(fg2);
        drawWrapped(p, body, bodyFont(), QRectF(inner.left(), y + 2, inner.width(), 0), compact ? 2 : 0);
    }
}

void NotificationsPane::paintEvent(QPaintEvent *)
{
    if (!m_plugin) return;
    const shade::Theme &t = shade::Theme::get();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    const QFontMetricsF fm(bodyFont());
    if (m_placeholder > 0.001) {
        // Slides up off-screen as the first notification arrives.
        p.save();
        p.setOpacity(m_placeholder);
        p.translate(0, -(1 - m_placeholder) * 16);
        p.setFont(bodyFont());
        if (!m_plugin->daemonRunning()) {
            p.setPen(t.tx3());
            p.drawText(QRectF(0, 0, width(), kPlaceholder), Qt::AlignCenter, QStringLiteral("another notification daemon is running."));
            p.restore();
            goto rows;
        }
        {
        const bool dnd = m_plugin->model()->dnd();
        const QString lead = dnd ? QString() : QStringLiteral("quiet. ");
        const QString link = placeholderLink();
        const qreal total = fm.horizontalAdvance(lead) + fm.horizontalAdvance(link);
        qreal x = (width() - total) / 2;
        const QRectF line(0, 0, width(), kPlaceholder);
        if (!lead.isEmpty()) {
            p.setPen(t.tx());
            p.drawText(QRectF(x, 0, fm.horizontalAdvance(lead) + 2, line.height()), Qt::AlignLeft | Qt::AlignVCenter, lead);
            x += fm.horizontalAdvance(lead);
        }
        QFont uf = bodyFont();
        uf.setUnderline(true);
        p.setFont(uf);
        p.setPen(dndLinkRect().contains(m_mouse) ? t.tx() : t.tx2());
        p.drawText(QRectF(x, 0, fm.horizontalAdvance(link) + 2, line.height()), Qt::AlignLeft | Qt::AlignVCenter, link);
        }
        p.restore();
    }
rows:

    p.save();
    p.setClipRect(QRectF(0, 0, width(), height() - kFooter * m_footer));
    // Paint bottom-up so a dying row fades over its neighbours.
    for (auto it = m_rows.crbegin(); it != m_rows.crend(); ++it) {
        if (it->opacity <= 0.001) continue;
        const Notification *n = m_plugin->model()->find(it->id);
        Notification ghost;
        if (!n) {
            // Already removed from the model (fading out): keep painting a
            // cached copy.
            ghost = m_ghosts.value(it->id);
            n = &ghost;
        }
        p.setOpacity(it->opacity);
        paintCard(&p, QRectF(kPad, it->y, kWidth - 2 * kPad, it->height), *n, it->hover, false);
    }
    p.restore();

    if (m_footer > 0.001) {
        const int total = int(m_plugin->model()->items().size());
        const int more = std::max(0, total - kMaxRows);
        p.setOpacity(m_footer);
        const QRectF f(kPad + 4, height() - kFooter, width() - 2 * kPad - 8, kFooter - 6);
        QFont uf = bodyFont();
        uf.setUnderline(true);
        p.setFont(uf);
        p.setPen(clearAllRect().contains(m_mouse) ? t.tx() : t.tx2());
        p.drawText(f, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("clear all"));
        p.setFont(bodyFont());
        p.setPen(t.tx2());
        p.drawText(f, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("+%1 more.").arg(more));
    }
}

NotificationsPane::Row *NotificationsPane::rowAt(const QPointF &pos)
{
    for (Row &r : m_rows) {
        if (!r.dying && !r.hidden && rowRect(r.id).contains(pos)) return &r;
    }
    return nullptr;
}

void NotificationsPane::mouseMoveEvent(QMouseEvent *e)
{
    m_mouse = e->position();
    const bool link = clearAllRect().contains(m_mouse) || dndLinkRect().contains(m_mouse);
    setCursor(link || rowAt(m_mouse) ? Qt::PointingHandCursor : Qt::ArrowCursor);
    ensureTicking();
}

void NotificationsPane::leaveEvent(QEvent *)
{
    m_mouse = QPointF(-1, -1);
    ensureTicking();
}

void NotificationsPane::mousePressEvent(QMouseEvent *e) { e->accept(); }

void NotificationsPane::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_plugin) return;
    const QPointF pos = e->position();
    if (clearAllRect().contains(pos)) {
        m_plugin->server()->clearAll();
        return;
    }
    if (dndLinkRect().contains(pos)) {
        m_plugin->model()->setDnd(!m_plugin->model()->dnd());
        return;
    }
    if (Row *r = rowAt(pos)) {
        const quint32 id = r->id;
        if (e->button() == Qt::LeftButton) m_plugin->dismiss(id, true);
        else if (e->button() == Qt::RightButton) m_plugin->dismiss(id, false);
    }
}

void NotificationsPane::showEvent(QShowEvent *)
{
    m_ageTimer->start();
    update();
}

void NotificationsPane::hideEvent(QHideEvent *) { m_ageTimer->stop(); }

// --- peek -----------------------------------------------------------------

namespace
{
Notification fromPeek(const shade::PeekView &peek)
{
    Notification n;
    n.id = peek.data.value(QStringLiteral("nid")).toUInt();
    n.app = peek.data.value(QStringLiteral("app")).toString();
    n.summary = peek.data.value(QStringLiteral("summary")).toString();
    n.body = peek.data.value(QStringLiteral("body")).toString();
    n.urgency = quint8(peek.data.value(QStringLiteral("urgency")).toUInt());
    n.received = QDateTime::fromMSecsSinceEpoch(peek.data.value(QStringLiteral("received")).toLongLong());
    return n;
}
constexpr int kPeekWidth = 340;
} // namespace

QSize NotificationsPane::peekSizeHint(const shade::PeekView &peek) const
{
    const Notification n = fromPeek(peek);
    const qreal inner = kPeekWidth - 2 * 6 - 2 * kCardPad;
    qreal h = kCardPad + QFontMetricsF(sourceFont()).height() + 2;
    h += drawWrapped(nullptr, n.summary, titleFont(), QRectF(0, 0, inner, 0), 1);
    const QString body = Model::truncateBody(n.body);
    if (!body.isEmpty()) h += 2 + drawWrapped(nullptr, body, bodyFont(), QRectF(0, 0, inner, 0), 2);
    return QSize(kPeekWidth, int(std::ceil(h + kCardPad + 12)));
}

void NotificationsPane::paintPeek(QPainter *p, const QRectF &rect, const shade::PeekView &peek, qreal)
{
    Notification n = fromPeek(peek);
    if (const Notification *live = m_plugin ? m_plugin->model()->find(n.id) : nullptr) n = *live; // fresh age
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QSize natural = peekSizeHint(peek);
    QRectF r(QPointF(0, 0), QSizeF(natural));
    r.moveCenter(rect.center());
    paintCard(p, r.adjusted(6, 6, -6, -6), n, 0, true);
    p->restore();
}

bool NotificationsPane::peekPointerEvent(QEvent::Type type, const QPointF &, Qt::MouseButton button, const shade::PeekView &peek)
{
    if (type != QEvent::MouseButtonRelease || !m_plugin) return type == QEvent::MouseButtonPress;
    const quint32 id = peek.data.value(QStringLiteral("nid")).toUInt();
    if (button == Qt::LeftButton) m_plugin->dismiss(id, true);
    else if (button == Qt::RightButton) m_plugin->dismiss(id, false);
    return true;
}

} // namespace notif
