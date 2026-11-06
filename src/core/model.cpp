#include "model.h"

#include <algorithm>
#include <cmath>

namespace shade::core
{

namespace
{
qreal lerp(qreal a, qreal b, qreal t) { return a + (b - a) * t; }
} // namespace

Expand parseExpand(const QString &s)
{
    if (s == QLatin1String("top")) return Expand::Top;
    if (s == QLatin1String("none")) return Expand::None;
    return Expand::Hover;
}

QString expandName(Expand e)
{
    switch (e) {
    case Expand::Top: return QStringLiteral("top");
    case Expand::None: return QStringLiteral("none");
    case Expand::Hover: break;
    }
    return QStringLiteral("hover");
}

// ---------------------------------------------------------------------------
// PeekQueue
// ---------------------------------------------------------------------------

bool PeekQueue::publish(PeekEntry entry)
{
    for (PeekEntry &e : m_entries) {
        if (e.sameIdentity(entry)) {
            // Replace in place: keep arrival order unless priority changed.
            entry.seq = e.seq;
            e = entry;
            sort();
            return false;
        }
    }
    entry.seq = ++m_seq;
    m_entries.push_back(entry);
    sort();
    return true;
}

QVector<PeekEntry> PeekQueue::dismiss(const QString &plugin, const QString &id)
{
    QVector<PeekEntry> removed;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->plugin == plugin && (id.isEmpty() || it->id == id)) {
            removed.push_back(*it);
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

std::optional<PeekEntry> PeekQueue::takeFront()
{
    if (m_entries.isEmpty()) return std::nullopt;
    PeekEntry e = m_entries.takeFirst();
    return e;
}

void PeekQueue::sort()
{
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const PeekEntry &a, const PeekEntry &b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.seq > b.seq; // younger first
    });
}

// ---------------------------------------------------------------------------
// Pager
// ---------------------------------------------------------------------------

void Pager::setCenters(const QVector<qreal> &centers)
{
    const qreal oldSelectedCenter = m_centers.value(m_selected, 0.0);
    const qreal offset = m_spring.value - oldSelectedCenter;
    m_centers = centers.isEmpty() ? QVector<qreal>{0.0} : centers;
    m_selected = std::clamp(m_selected, 0, int(m_centers.size()) - 1);
    // Keep the strip where it was relative to the selected pane.
    m_spring.target = m_centers[m_selected];
    m_spring.value = m_spring.target + (m_gesture ? offset : 0.0);
    if (!m_gesture) m_spring.velocity = 0.0;
    m_anchor = m_centers[std::clamp(m_gestureSection, 0, int(m_centers.size()) - 1)];
}

qreal Pager::stickyOffset(qreal pull) const
{
    const qreal d = m_s->stickyDistance, r = m_s->stickyResistance;
    const qreal m = std::abs(pull);
    if (m <= d) return pull * r;
    return std::copysign(d * r + (m - d), pull);
}

qreal Pager::inverseStickyOffset(qreal offset) const
{
    const qreal d = m_s->stickyDistance, r = m_s->stickyResistance;
    const qreal stuck = d * r;
    const qreal m = std::abs(offset);
    if (m <= stuck) return offset / r;
    return std::copysign(d + (m - stuck), offset);
}

qreal Pager::rubberBand(qreal excess) const
{
    const qreal max = m_s->overscroll;
    if (max <= 0) return 0;
    return max * (1.0 - std::exp(-excess * m_s->overscrollResistance / max));
}

int Pager::nearest(qreal position) const
{
    int best = 0;
    for (int i = 1; i < m_centers.size(); ++i) {
        if (std::abs(position - m_centers[i]) < std::abs(position - m_centers[best])) best = i;
    }
    return best;
}

void Pager::scroll(qreal delta, qreal now)
{
    const qreal previous = m_spring.value;
    if (!m_gesture) {
        // Grabbing a strip that is still snapping takes it over without the
        // sticky break-in.
        const bool moving = !m_spring.settled() || m_spring.value != m_spring.target;
        m_gesture = true;
        m_stuck = !moving;
        m_gestureSection = nearest(m_spring.value);
        m_anchor = m_spring.value;
        m_pull = 0.0;
        m_velocity = 0.0;
        m_lastInput = -1.0;
    }
    const qreal dt = m_lastInput >= 0 ? now - m_lastInput : 0.0;
    m_lastInput = now;
    m_pull += delta;

    // One gesture moves at most one pane: the strip is limited to the panes
    // next to where the gesture started; beyond that it rubber-bands.
    const int last = int(m_centers.size()) - 1;
    const qreal prevLimit = m_gestureSection > 0 ? m_centers[m_gestureSection - 1] - m_anchor : m_centers[0] - m_anchor;
    const qreal nextLimit = m_gestureSection < last ? m_centers[m_gestureSection + 1] - m_anchor : m_centers[last] - m_anchor;
    const qreal reach = m_s->overscroll * 4.0;

    qreal offset = m_stuck ? stickyOffset(m_pull) : m_pull;
    offset = std::clamp(offset, prevLimit - reach, nextLimit + reach);
    m_pull = m_stuck ? inverseStickyOffset(offset) : offset;
    if (offset > nextLimit) offset = nextLimit + rubberBand(offset - nextLimit);
    else if (offset < prevLimit) offset = prevLimit - rubberBand(prevLimit - offset);

    const qreal position = m_anchor + offset;
    m_spring.value = position;
    m_spring.target = position;
    if (dt > 0) {
        const qreal instant = (position - previous) / dt;
        m_velocity = lerp(m_velocity, instant, 0.55);
        m_velocity = std::clamp(m_velocity, -5000.0, 5000.0);
    }
    m_spring.velocity = m_velocity;
}

void Pager::release()
{
    if (!m_gesture) return;
    m_gesture = false;
    m_pull = 0.0;
    m_stuck = true;

    int target = m_gestureSection;
    const qreal moved = m_spring.value - m_centers[m_gestureSection];
    const int dir = moved > 0 ? 1 : -1;
    const int neighbour = std::clamp(m_gestureSection + dir, 0, int(m_centers.size()) - 1);
    if (neighbour != m_gestureSection && std::abs(moved) > 0.0) {
        const qreal span = std::abs(m_centers[neighbour] - m_centers[m_gestureSection]);
        const bool flick = std::abs(m_velocity) >= m_s->flickSpeed && (m_velocity > 0) == (dir > 0);
        if (flick || std::abs(moved) > span * m_s->advanceFraction) target = neighbour;
    }
    m_selected = target;
    m_spring.target = m_centers[target];
    // Keep the release velocity, but only towards the target: a strip pulled
    // the wrong way then released should not fling past it.
    m_spring.velocity = (m_spring.target - m_spring.value) * m_velocity >= 0 ? m_velocity : m_velocity * 0.2;
}

void Pager::select(int index, bool animate)
{
    index = std::clamp(index, 0, int(m_centers.size()) - 1);
    m_gesture = false;
    m_selected = index;
    m_spring.target = m_centers[index];
    if (!animate) m_spring.snap();
}

void Pager::step(qreal dt)
{
    if (m_gesture) return;
    m_spring.stiffness = m_s->scrollStiffness;
    m_spring.damping = m_s->scrollDamping;
    m_spring.epsilon = 0.05;
    m_spring.step(dt);
}

// ---------------------------------------------------------------------------
// ShadeModel
// ---------------------------------------------------------------------------

ShadeModel::ShadeModel(const Settings *settings, QObject *parent)
    : QObject(parent)
    , m_s(settings)
    , m_pager(settings)
{
    m_width.reset(m_s->barWidth);
    m_height.reset(m_s->barHeight);
    m_top.reset(0.0);
    m_morph.reset(1.0);
    m_morph.epsilon = 0.002;
    m_swap.reset(0.0);
    m_swap.epsilon = 0.002;
    retarget();
}

QString ShadeModel::stateName() const
{
    switch (m_state) {
    case State::Bar: return QStringLiteral("bar");
    case State::Peek: return QStringLiteral("peek");
    case State::Widgets: return QStringLiteral("widgets");
    }
    return {};
}

void ShadeModel::setPanes(const QVector<PaneInfo> &panes)
{
    const QString selectedName = selectedPlugin();
    m_panes = panes;
    QVector<qreal> centers;
    qreal c = 0.0;
    for (int i = 0; i < m_panes.size(); ++i) {
        if (i > 0) c += (m_panes[i - 1].size.width() + m_panes[i].size.width()) / 2.0 + m_s->sectionGap;
        centers.push_back(c);
    }
    m_pager.setCenters(centers);
    const int keep = paneIndex(selectedName);
    if (keep >= 0 && keep != m_pager.selected()) m_pager.select(keep, false);
    retarget();
    updateActive();
    emit wakeUp();
}

int ShadeModel::paneIndex(const QString &plugin) const
{
    for (int i = 0; i < m_panes.size(); ++i) {
        if (m_panes[i].plugin == plugin) return i;
    }
    return -1;
}

QString ShadeModel::selectedPlugin() const
{
    const int i = m_pager.selected();
    return i >= 0 && i < m_panes.size() ? m_panes[i].plugin : QString();
}

QString ShadeModel::activePlugin() const { return m_state == State::Widgets ? selectedPlugin() : QString(); }

QSizeF ShadeModel::sizeAt(qreal position) const
{
    if (m_panes.isEmpty()) return QSizeF(m_s->barWidth, 40);
    if (m_panes.size() == 1) return m_panes[0].size;
    const QVector<qreal> &centers = m_pager.centers();
    const int last = int(m_panes.size()) - 1;
    // Linear between neighbours, extrapolated past the ends, so overshooting
    // the snap (or rubber-banding) makes the size bounce with the strip.
    int i = 0;
    while (i < last - 1 && position > centers[i + 1]) ++i;
    const qreal span = centers[i + 1] - centers[i];
    const qreal t = span > 0 ? (position - centers[i]) / span : 0.0;
    const QSizeF a = m_panes[i].size, b = m_panes[i + 1].size;
    return QSizeF(std::max<qreal>(1.0, lerp(a.width(), b.width(), t)),
                  std::max<qreal>(1.0, lerp(a.height(), b.height(), t)));
}

const PeekEntry *ShadeModel::presentedPeek() const
{
    return m_queue.front();
}

QRectF ShadeModel::pillRect() const
{
    const qreal w = std::max<qreal>(0.0, m_width.value);
    const qreal h = std::max<qreal>(0.0, m_height.value);
    return QRectF(m_canvasWidth / 2.0 - w / 2.0, m_top.value, w, h);
}

qreal ShadeModel::radius() const
{
    const QRectF r = pillRect();
    return std::max<qreal>(0.0, std::min({m_s->cornerRadius, r.height() / 2.0, r.width() / 2.0}));
}

qreal ShadeModel::contentOpacity() const
{
    if (m_s->topOffset <= 0) return 1.0;
    const qreal open = std::clamp(m_top.value / m_s->topOffset, 0.0, 1.0);
    const qreal t = std::clamp((open - 0.25) / 0.75, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

QRectF ShadeModel::paneRect(int i) const
{
    if (i < 0 || i >= m_panes.size()) return {};
    const QRectF pill = pillRect();
    const QSizeF s = m_panes[i].size;
    const qreal cx = pill.center().x() + m_pager.centers().value(i) - m_pager.position();
    return QRectF(cx - s.width() / 2.0, pill.center().y() - s.height() / 2.0, s.width(), s.height());
}

QRectF ShadeModel::targetPeekRect() const
{
    const PeekEntry *f = m_queue.front();
    if (!f) return {};
    return QRectF(m_canvasWidth / 2.0 - f->size.width() / 2.0, m_s->topOffset, f->size.width(), f->size.height());
}

void ShadeModel::setState(State s)
{
    if (s == m_state) return;
    m_state = s;
    m_follow = false;
    if (s != State::Peek) m_peekHovered = false;
    retarget();
    emit stateChanged(s);
    updateActive();
    emit wakeUp();
}

void ShadeModel::updateActive()
{
    const QString a = activePlugin();
    if (a != m_active) {
        m_active = a;
        emit activePluginChanged(a);
    }
}

void ShadeModel::retarget()
{
    const bool collapsing = m_state == State::Bar;
    const qreal k = collapsing ? m_s->collapseStiffness : m_s->expandStiffness;
    const qreal z = collapsing ? m_s->collapseDamping : m_s->expandDamping;
    for (Spring *sp : {&m_width, &m_height, &m_top}) {
        sp->stiffness = k;
        sp->damping = z;
        sp->epsilon = 0.05;
    }
    m_morph.stiffness = m_s->morphStiffness;
    m_morph.damping = m_s->morphDamping;
    m_swap.stiffness = m_s->morphStiffness * 1.4;
    m_swap.damping = 1.0;

    switch (m_state) {
    case State::Bar:
        m_width.target = m_s->barWidth;
        m_height.target = m_s->barHeight;
        m_top.target = 0.0;
        break;
    case State::Peek: {
        const PeekEntry *f = m_queue.front();
        const QSizeF sz = f ? f->size : QSizeF(m_s->barWidth, 30);
        m_width.target = sz.width();
        m_height.target = sz.height();
        m_top.target = m_s->topOffset;
        m_morph.target = 0.0;
        break;
    }
    case State::Widgets: {
        const QSizeF sz = sizeAt(m_pager.position());
        m_width.target = sz.width();
        m_height.target = sz.height();
        m_top.target = m_s->topOffset;
        m_morph.target = 1.0;
        if (m_follow) {
            // Settled: track the strip tightly (the strip's own spring
            // provides the bounce), critically damped so it never lags.
            for (Spring *sp : {&m_width, &m_height}) {
                sp->stiffness = 2400.0;
                sp->damping = 1.0;
            }
        }
        break;
    }
    }
}

void ShadeModel::pointerMoved(const QPointF &pos)
{
    m_lastPointer = m_now;
    switch (m_state) {
    case State::Bar: {
        const qreal half = m_s->barWidth / 2.0 + 8.0;
        if (pos.y() <= m_s->hoverZone && std::abs(pos.x() - m_canvasWidth / 2.0) <= half) expand();
        break;
    }
    case State::Peek: {
        const PeekEntry *f = m_queue.front();
        if (!f) break;
        const QRectF r = targetPeekRect().adjusted(-2, -m_s->topOffset, 2, 2);
        const bool inside = r.contains(pos);
        m_peekHovered = inside;
        if (f->expand == Expand::Hover && inside && pos.y() >= m_s->topOffset - 1) {
            reveal(f->plugin);
        } else if (f->expand != Expand::None && pos.y() <= m_s->revealZone && inside) {
            reveal(f->plugin);
        }
        break;
    }
    case State::Widgets:
        break;
    }
}

void ShadeModel::pointerLeft()
{
    m_peekHovered = false;
    if (m_pager.gestureActive()) m_pager.release();
    if (m_state == State::Widgets) collapse();
}

void ShadeModel::expand()
{
    if (m_state == State::Peek) {
        const PeekEntry *f = m_queue.front();
        reveal(f ? f->plugin : QString());
        return;
    }
    setState(State::Widgets);
}

void ShadeModel::collapse()
{
    if (m_pager.gestureActive()) m_pager.release();
    setState(m_queue.isEmpty() ? State::Bar : State::Peek);
}

void ShadeModel::reveal(const QString &plugin)
{
    const int idx = paneIndex(plugin);
    // While the strip is invisible (pure peek), jump to the owner's pane so
    // the peek expands into its own widget.
    if (idx >= 0 && m_morph.value < 0.5) {
        m_pager.select(idx, false);
        emit selectedChanged(idx);
    } else if (idx >= 0) {
        m_pager.select(idx, true);
        emit selectedChanged(idx);
    }
    setState(State::Widgets);
}

bool ShadeModel::focusPlugin(const QString &plugin)
{
    const int idx = paneIndex(plugin);
    if (idx < 0) return false;
    if (m_state == State::Widgets) {
        if (idx != m_pager.selected()) {
            m_pager.select(idx, true);
            emit selectedChanged(idx);
            updateActive();
            emit wakeUp();
        }
        return true;
    }
    const bool invisible = m_state == State::Bar ? contentOpacity() < 0.05 : m_morph.value < 0.5;
    m_pager.select(idx, !invisible);
    emit selectedChanged(idx);
    setState(State::Widgets);
    return true;
}

bool ShadeModel::wheel(qreal dx, qreal dy, bool gestureEnd)
{
    m_lastPointer = m_now;
    if (m_state == State::Widgets) {
        if (gestureEnd) {
            if (m_pager.gestureActive()) {
                const int before = m_pager.selected();
                m_pager.release();
                if (m_pager.selected() != before) {
                    emit selectedChanged(m_pager.selected());
                    updateActive();
                }
                emit wakeUp();
                return true;
            }
            return false;
        }
        if (std::abs(dx) <= std::abs(dy) && !m_pager.gestureActive()) return false; // vertical: pane's
        if (qFuzzyIsNull(dx)) return m_pager.gestureActive();
        m_follow = true;
        m_pager.scroll(dx * m_s->scrollDirection, m_now);
        emit wakeUp();
        return true;
    }
    if (m_state == State::Peek) {
        if (gestureEnd) {
            endPeekGesture();
            return true;
        }
        m_peekLastWheel = m_now;
        if (std::abs(dy) > std::abs(dx)) m_peekScroll += dy;
        if (!m_peekGestureUsed && m_peekScroll >= m_s->peekDismissDistance) {
            // Scroll-up retires exactly one peek per gesture.
            m_peekGestureUsed = true;
            if (const PeekEntry *f = m_queue.front()) dismissPeek(f->plugin, f->id, QStringLiteral("user"));
        }
        emit wakeUp();
        return true;
    }
    return false;
}

void ShadeModel::endPeekGesture()
{
    m_peekScroll = 0.0;
    m_peekGestureUsed = false;
    m_peekLastWheel = -1.0;
}

void ShadeModel::publishPeek(PeekEntry entry)
{
    const std::optional<PeekEntry> before = m_queue.front() ? std::optional<PeekEntry>(*m_queue.front()) : std::nullopt;
    entry.remainingMs = entry.timeoutMs;
    m_queue.publish(entry);
    frontChanged(before);
}

void ShadeModel::dismissPeek(const QString &plugin, const QString &id, const QString &reason)
{
    const std::optional<PeekEntry> before = m_queue.front() ? std::optional<PeekEntry>(*m_queue.front()) : std::nullopt;
    const QVector<PeekEntry> removed = m_queue.dismiss(plugin, id);
    if (removed.isEmpty()) return;
    frontChanged(before);
    for (const PeekEntry &e : removed) emit peekRemoved(e.plugin, e.id, reason);
}

void ShadeModel::updatePeekSize(const QString &plugin, const QString &id, const QSizeF &size)
{
    for (PeekEntry &e : m_queue.entries()) {
        if (e.plugin == plugin && e.id == id) e.size = size;
    }
    retarget();
    emit wakeUp();
}

void ShadeModel::frontChanged(const std::optional<PeekEntry> &before)
{
    const PeekEntry *now = m_queue.front();
    switch (m_state) {
    case State::Bar:
        if (now) setState(State::Peek);
        break;
    case State::Peek:
        if (!now) {
            setState(State::Bar);
        } else if (before && !before->sameIdentity(*now)) {
            // Front changed: crossfade old -> new while the size morphs.
            m_outgoing = before;
            m_swap.reset(1.0);
            m_swap.target = 0.0;
        }
        retarget();
        break;
    case State::Widgets:
        // Queue bookkeeping only; nothing visible changes.
        break;
    }
    emit wakeUp();
}

bool ShadeModel::geometrySettled() const
{
    if (m_pager.gestureActive() || !m_pager.settled()) return false;
    for (const Spring *sp : {&m_width, &m_height, &m_top, &m_morph, &m_swap}) {
        if (!sp->settled() || sp->value != sp->target) return false;
    }
    return true;
}

bool ShadeModel::restingOnPane() const
{
    return m_state == State::Widgets && m_follow && !m_pager.gestureActive() && m_pager.settled() &&
           m_morph.value >= 0.999 && m_top.settled() && m_top.value == m_top.target && !m_outgoing;
}

bool ShadeModel::needsTick() const
{
    if (m_pager.gestureActive() || !m_pager.settled()) return true;
    if (m_peekLastWheel >= 0) return true;
    for (const Spring *sp : {&m_width, &m_height, &m_top, &m_morph, &m_swap}) {
        if (!sp->settled() || sp->value != sp->target) return true;
    }
    // Peek timers.
    if (m_state != State::Widgets) {
        for (const PeekEntry &e : m_queue.entries()) {
            if (e.timeoutMs > 0) return true;
        }
    } else {
        for (const PeekEntry &e : m_queue.entries()) {
            if (e.timeoutMs > 0) return true;
        }
    }
    if (m_s->idleTimeoutMs > 0 && m_state == State::Widgets) return true;
    return false;
}

bool ShadeModel::tick(qreal dt)
{
    dt = std::clamp(dt, 0.0, 0.1);
    m_now += dt;

    // Gesture idle -> release.
    if (m_pager.gestureActive() && m_now - m_pager.lastInput() > m_s->scrollReleaseMs / 1000.0) {
        const int before = m_pager.selected();
        m_pager.release();
        if (m_pager.selected() != before) {
            emit selectedChanged(m_pager.selected());
            updateActive();
        }
    }
    if (m_peekLastWheel >= 0 && m_now - m_peekLastWheel > 0.25) endPeekGesture();

    // Peek timers: run unless the user is hovering the presented peek.
    QVector<PeekEntry> expired;
    for (PeekEntry &e : m_queue.entries()) {
        if (e.timeoutMs <= 0) continue;
        const bool presentedHover = m_state == State::Peek && m_peekHovered && m_queue.front() && m_queue.front()->sameIdentity(e);
        if (presentedHover) continue;
        e.remainingMs -= dt * 1000.0;
        if (e.remainingMs <= 0) expired.push_back(e);
    }
    for (const PeekEntry &e : expired) dismissPeek(e.plugin, e.id, QStringLiteral("timeout"));

    if (m_s->idleTimeoutMs > 0 && m_state == State::Widgets &&
        (m_now - m_lastPointer) * 1000.0 > m_s->idleTimeoutMs) {
        collapse();
    }

    m_pager.step(dt);
    retarget();
    for (Spring *sp : {&m_width, &m_height, &m_top, &m_morph, &m_swap}) sp->step(dt);
    if (m_state == State::Widgets && !m_follow &&
        std::abs(m_width.value - m_width.target) < 1.0 && std::abs(m_height.value - m_height.target) < 1.0 &&
        std::abs(m_width.velocity) < 30 && std::abs(m_height.velocity) < 30) {
        m_follow = true;
    }
    if (m_outgoing && m_swap.value <= 0.001) m_outgoing.reset();
    return needsTick();
}

} // namespace shade::core
