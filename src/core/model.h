#pragma once

#include "settings.h"
#include "spring.h"

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QVariantMap>
#include <QVector>

#include <optional>

namespace shade::core
{

// ---------------------------------------------------------------------------
// Peek queue
// ---------------------------------------------------------------------------

enum class Expand { Hover, Top, None };

Expand parseExpand(const QString &s);
QString expandName(Expand e);

struct PeekEntry
{
    QString plugin;
    QString id;
    int priority = 2;
    Expand expand = Expand::Hover;
    int timeoutMs = 0; // 0 = until dismissed
    qreal remainingMs = 0;
    quint64 seq = 0;
    QVariantMap data;
    QSizeF size{200, 34};

    bool sameIdentity(const PeekEntry &o) const { return plugin == o.plugin && id == o.id; }
};

// Sorted by priority (high first), ties: younger first.
class PeekQueue
{
public:
    // Insert or replace in place. Returns true when it is a new entry.
    bool publish(PeekEntry entry);
    // Removes matching entries (empty id = all of plugin). Returns removed.
    QVector<PeekEntry> dismiss(const QString &plugin, const QString &id);
    std::optional<PeekEntry> takeFront();

    const PeekEntry *front() const { return m_entries.isEmpty() ? nullptr : &m_entries.front(); }
    PeekEntry *front() { return m_entries.isEmpty() ? nullptr : &m_entries.front(); }
    bool isEmpty() const { return m_entries.isEmpty(); }
    int size() const { return m_entries.size(); }
    const QVector<PeekEntry> &entries() const { return m_entries; }
    QVector<PeekEntry> &entries() { return m_entries; }

private:
    void sort();
    QVector<PeekEntry> m_entries;
    quint64 m_seq = 0;
};

// ---------------------------------------------------------------------------
// Pager: one pane per gesture with a rubber band
// ---------------------------------------------------------------------------

class Pager
{
public:
    explicit Pager(const Settings *settings) : m_s(settings) {}

    void setCenters(const QVector<qreal> &centers);
    const QVector<qreal> &centers() const { return m_centers; }

    // A horizontal delta (already direction-corrected) at time `now` (s).
    void scroll(qreal delta, qreal now);
    // Gesture ended (fingers lifted / idle): settle on a pane.
    void release();
    bool gestureActive() const { return m_gesture; }
    qreal lastInput() const { return m_lastInput; }

    void step(qreal dt);
    bool settled() const { return !m_gesture && m_spring.settled() && m_spring.value == m_spring.target; }

    qreal position() const { return m_spring.value; }
    int selected() const { return m_selected; }
    int nearest(qreal position) const;
    // Programmatic selection (animated or instant).
    void select(int index, bool animate);

private:
    qreal stickyOffset(qreal pull) const;
    qreal inverseStickyOffset(qreal offset) const;
    qreal rubberBand(qreal excess) const;

    const Settings *m_s;
    QVector<qreal> m_centers{0.0};
    Spring m_spring;
    int m_selected = 0;

    bool m_gesture = false;
    bool m_stuck = true;
    int m_gestureSection = 0;
    qreal m_anchor = 0.0;
    qreal m_pull = 0.0;
    qreal m_velocity = 0.0;
    qreal m_lastInput = -1.0;
};

// ---------------------------------------------------------------------------
// The shade model: one per window. Pure logic, no GUI; driven by tick().
// ---------------------------------------------------------------------------

struct PaneInfo
{
    QString plugin;
    QSizeF size;
};

class ShadeModel : public QObject
{
    Q_OBJECT

public:
    enum class State { Bar, Peek, Widgets };
    Q_ENUM(State)

    explicit ShadeModel(const Settings *settings, QObject *parent = nullptr);

    // --- configuration ----------------------------------------------------
    void setCanvasWidth(qreal width) { m_canvasWidth = width; }
    qreal canvasWidth() const { return m_canvasWidth; }
    void setPanes(const QVector<PaneInfo> &panes);
    const QVector<PaneInfo> &panes() const { return m_panes; }
    int paneIndex(const QString &plugin) const;

    // --- input -------------------------------------------------------------
    void pointerMoved(const QPointF &pos);
    void pointerLeft();
    // dx/dy in pixels (positive dx = towards the next pane, positive dy =
    // scroll up). Returns true when the shade consumed it.
    bool wheel(qreal dx, qreal dy, bool gestureEnd = false);
    void expand();
    void collapse();
    bool focusPlugin(const QString &plugin);

    // --- peeks -------------------------------------------------------------
    void publishPeek(PeekEntry entry);
    void dismissPeek(const QString &plugin, const QString &id, const QString &reason = QStringLiteral("dismissed"));
    void updatePeekSize(const QString &plugin, const QString &id, const QSizeF &size);
    const PeekQueue &queue() const { return m_queue; }

    // --- time ---------------------------------------------------------------
    // Advance by dt seconds. Returns true while anything is still moving.
    bool tick(qreal dt);
    bool needsTick() const;
    // No spring is moving and no gesture is in progress (timers ignored).
    bool geometrySettled() const;
    // Expanded and resting on a pane: only the pane's own size may still be
    // animating (no expand/collapse, paging or peek morph in progress).
    bool restingOnPane() const;
    qreal now() const { return m_now; }

    // --- output -------------------------------------------------------------
    State state() const { return m_state; }
    QString stateName() const;
    int selected() const { return m_pager.selected(); }
    QString selectedPlugin() const;
    // The pane that is "active" (lifecycle): selected pane while expanded.
    QString activePlugin() const;

    QRectF pillRect() const;
    qreal radius() const;
    qreal scrollPosition() const { return m_pager.position(); }
    qreal morph() const { return m_morph.value; }
    qreal contentOpacity() const;
    // Rect of pane i in canvas coordinates (at its natural size).
    QRectF paneRect(int i) const;
    const PeekEntry *presentedPeek() const;
    // While the front peek changes: the previous one and its fading opacity.
    const PeekEntry *outgoingPeek() const { return m_outgoing ? &*m_outgoing : nullptr; }
    qreal outgoingOpacity() const { return m_swap.value; }
    bool peekHovered() const { return m_peekHovered; }
    QSizeF sizeAt(qreal position) const;
    const Pager &pager() const { return m_pager; }

signals:
    void stateChanged(shade::core::ShadeModel::State state);
    void selectedChanged(int index);
    void activePluginChanged(const QString &plugin);
    void peekRemoved(const QString &plugin, const QString &id, const QString &reason);
    void wakeUp(); // something needs ticking again

private:
    void setState(State s);
    void retarget();
    void frontChanged(const std::optional<PeekEntry> &previousFront);
    void reveal(const QString &plugin);
    void updateActive();
    QRectF targetPeekRect() const;
    void endPeekGesture();

    const Settings *m_s;
    State m_state = State::Bar;
    QVector<PaneInfo> m_panes;
    qreal m_canvasWidth = 600.0;
    Pager m_pager;
    PeekQueue m_queue;

    Spring m_width, m_height, m_top, m_morph, m_swap;
    std::optional<PeekEntry> m_outgoing;
    std::optional<PeekEntry> m_lastFront;

    bool m_peekHovered = false;
    bool m_follow = false; // size follows the strip tightly (settled expand)
    qreal m_now = 0.0;
    qreal m_lastPointer = 0.0;

    // Scroll-up on a peek: one dismissal per gesture.
    qreal m_peekScroll = 0.0;
    bool m_peekGestureUsed = false;
    qreal m_peekLastWheel = -1.0;

    QString m_active;
};

} // namespace shade::core
