#pragma once

#include <shade/icon.h>
#include <shade/pane.h>

#include "model.h"

#include <QElapsedTimer>
#include <QHash>
#include <QPointer>

class QTimer;

namespace notif
{

class NotificationsPlugin;

// The notification list: cards newest first (at most five, then a "+x
// more." footer with "clear all"), a "quiet." placeholder with a
// do-not-disturb link, staggered dismissal waves and a clear-all sweep.
// Everything is painted; each row has its own position/opacity springs and
// the pane height follows the animated content.
class NotificationsPane : public shade::PaneWidget
{
    Q_OBJECT

public:
    explicit NotificationsPane(NotificationsPlugin *plugin, QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize peekSizeHint(const shade::PeekView &peek) const override;
    void paintPeek(QPainter *painter, const QRectF &rect, const shade::PeekView &peek, qreal morph) override;
    bool peekPointerEvent(QEvent::Type type, const QPointF &pos, Qt::MouseButton button, const shade::PeekView &peek) override;

    static constexpr int kWidth = 400;
    static constexpr int kMaxRows = 5;

    // For tests: ids of visible rows (top to bottom) and their rects.
    QList<quint32> visibleIds() const;
    QRectF rowRect(quint32 id) const;
    QRectF clearAllRect() const;
    QRectF dndLinkRect() const;
    bool animating() const;

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    struct Row
    {
        quint32 id = 0;
        qreal y = 0, vy = 0, targetY = 0;
        qreal opacity = 0, targetOpacity = 1;
        qreal height = 0;
        qint64 startAt = 0; // ms on m_clock; the row holds still until then
        bool dying = false;
        bool hidden = false; // beyond the row cap
        qreal hover = 0;
    };

    void rebuild(bool animate);
    void relayout(qint64 staggerFrom = -1, int staggerIndex = 0);
    void onAdded(quint32 id);
    void onRemoved(quint32 id);
    void onCleared();
    void step();
    void ensureTicking();
    qreal contentHeight() const;
    int targetHeight() const;
    qreal cardHeight(const Notification &n, qreal width) const;
    void paintCard(QPainter *p, const QRectF &r, const Notification &n, qreal hover, bool compact) const;
    Row *rowAt(const QPointF &pos);
    QString placeholderLink() const;

    QPointer<NotificationsPlugin> m_plugin;
    QHash<quint32, Notification> m_ghosts; // copies for rows fading out after removal
    QList<Row> m_rows; // display order (top first), dying rows included
    QTimer *m_tick;
    QTimer *m_ageTimer;
    QElapsedTimer m_clock;
    qint64 m_last = 0;
    qreal m_height = 0, m_vh = 0;
    qreal m_placeholder = 1, m_footer = 0;
    QPointF m_mouse{-1, -1};
    int m_lastReported = -1;
};

} // namespace notif
