#pragma once

// A plugin's widget. One PaneWidget is delivered per window. It is a normal
// QWidget (layouts, child widgets, custom painting all work); the host
// composites it into the shade's pill, clips it to the rounded shape and
// forwards pointer input to it.
//
// The same widget also renders the plugin's *peeks*: a peek is not a separate
// widget but the "mini" presentation of this one. While a peek is presented
// the host calls paintPeek() and peekPointerEvent(). When the peek expands
// into the full widget (hover, cursor to top, activate_widget) the host
// animates morph() from 0 to 1 and crossfades paintPeek() into the widget.

#include <shade/sdk.h>

#include <QEvent>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QWidget>

class QPainter;

namespace shade
{

struct PeekView
{
    QString id;
    QVariantMap data;
};

class SHADE_SDK_EXPORT PaneWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PaneWidget(QWidget *parent = nullptr);
    ~PaneWidget() override;

    // Natural size of a peek. Called when the peek is published and whenever
    // the plugin calls peekGeometryChanged().
    virtual QSize peekSizeHint(const PeekView &peek) const;

    // Paint the peek into `rect` (the pill's current bounds, in painter
    // coordinates). `morph` is 0 while presented as a peek and rises to 1 as
    // the peek expands into the full widget; the host already applies the
    // crossfade opacity, so most plugins can ignore it.
    virtual void paintPeek(QPainter *painter,
                           const QRectF &rect,
                           const PeekView &peek,
                           qreal morph);

    // Pointer input inside a presented peek (positions are relative to the
    // peek rect). Return true when handled. Hover/expansion and scroll-up
    // dismissal are handled by the host before this is called.
    virtual bool peekPointerEvent(QEvent::Type type,
                                  const QPointF &pos,
                                  Qt::MouseButton button,
                                  const PeekView &peek);

    // The pill's corner radius. At rest the widget is shown live and is
    // clipped to a rectangle only: a widget that paints its own background
    // edge-to-edge should round it with this radius (see roundedRect()).
    qreal cornerRadius() const { return m_radius; }
    QPainterPath roundedRect() const;

    // Tell the host that sizeHint() changed (e.g. a list grew). The host also
    // polls sizeHint(), this just makes the resize immediate.
    void notifySizeChanged() { emit paneSizeChanged(); }

    // Ask the host to repaint the presented peek (e.g. an icon inside it is
    // morphing). Plain update() does not reach a peek: the widget itself is
    // hidden while it is presented as a peek.
    void requestPeekRepaint() { emit peekRepaintRequested(); }

    // 0 = shown as peek, 1 = shown as the full widget.
    qreal morph() const { return m_morph; }
    // True while this widget is the selected, expanded pane.
    bool isShadeActive() const { return m_active; }

    // Host side. Not for plugins.
    void hostSetMorph(qreal morph);
    void hostSetActive(bool active);
    void hostSetCornerRadius(qreal radius);

signals:
    // Emit when peekSizeHint() would now return something different.
    void peekGeometryChanged();
    void morphChanged(qreal morph);
    void shadeActiveChanged(bool active);
    void paneSizeChanged();
    void peekRepaintRequested();

private:
    qreal m_morph = 1.0;
    bool m_active = false;
    qreal m_radius = 18.0;
};

} // namespace shade
