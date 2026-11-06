#pragma once

// SVG icons with geometric morphing.
//
// Icons are plain SVG files (usually compiled into a plugin's Qt resources).
// Supported elements: path, circle, ellipse, rect (incl. rx), line, polyline,
// polygon, nested <g> (attributes inherit; translate/scale transforms).
// Every subpath of an element is one *contour*.
//
// Changing an icon's source morphs the geometry: contours with the same key
// interpolate point by point; a contour present on one side only grows from /
// shrinks into its anchor. A contour's key is the element's `id` (plus
// "#n" for its n-th subpath); elements without an id are keyed by order. The
// anchor is `data-anchor="x y"` on the element, or the contour's centroid.
//
// Color: icons are symbolic — every fill/stroke is drawn in the color passed
// to paint() (opacity, fill-opacity and stroke-opacity are honoured), except
// elements with `data-color="<theme color name>"` (e.g. "red", "green").

#include <shade/sdk.h>

#include <QColor>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QObject>
#include <QPainterPath>
#include <QPointer>
#include <QSharedPointer>
#include <QVector>
#include <QWidget>

class QPainter;
class QTimer;

namespace shade
{

struct IconContour
{
    QString key;
    QPainterPath path;      // exact geometry (used when not animating)
    QVector<QPointF> points; // arc-length samples (used for morphing)
    bool closed = false;
    QPointF anchor;
    qreal strokeWidth = 0.0; // 0 = no stroke
    qreal strokeOpacity = 1.0;
    qreal fillOpacity = 0.0; // 0 = no fill
    Qt::PenCapStyle cap = Qt::FlatCap;
    Qt::PenJoinStyle join = Qt::MiterJoin;
    QString colorRole; // theme color name, empty = paint color
};

struct SHADE_SDK_EXPORT VectorIcon
{
    QRectF viewBox{0, 0, 16, 16};
    QVector<IconContour> contours;
    bool isNull() const { return contours.isEmpty(); }

    // Parse SVG markup. Returns a null icon on failure (with a warning).
    static VectorIcon fromSvg(const QByteArray &svg, const QString &origin = {});
    // Load (cached) from a file or ":/resource" path.
    static QSharedPointer<const VectorIcon> load(const QString &path);
    // Number of samples per contour used for morphing.
    static constexpr int kSamples = 72;
};

// Pure function: geometry between `from` (t=0) and `to` (t=1). Exposed for
// tests and for plugins that drive their own timelines.
SHADE_SDK_EXPORT VectorIcon interpolateIcons(const VectorIcon &from,
                                            const VectorIcon &to,
                                            qreal t);

SHADE_SDK_EXPORT void paintIcon(QPainter *painter,
                                const VectorIcon &icon,
                                const QRectF &target,
                                const QColor &color,
                                bool exact = true);

// A morphing icon that is painted by its owner (inside paintEvent or a
// peek). Emits changed() on every animation frame.
class SHADE_SDK_EXPORT MorphIcon : public QObject
{
    Q_OBJECT

public:
    explicit MorphIcon(QObject *parent = nullptr);
    explicit MorphIcon(const QString &source, QObject *parent = nullptr);

    QString source() const { return m_source; }
    // Morph to a new icon. `animate=false` jumps.
    void setSource(const QString &source, bool animate = true);

    int duration() const { return m_duration; }
    void setDuration(int ms) { m_duration = ms; }
    void setEasing(const QEasingCurve &curve) { m_easing = curve; }

    bool isAnimating() const;
    void paint(QPainter *painter, const QRectF &target, const QColor &color) const;
    // The current (possibly mid-morph) geometry.
    VectorIcon current() const;

signals:
    void changed();

private:
    void step();

    QString m_source;
    QSharedPointer<const VectorIcon> m_target;
    VectorIcon m_from;
    int m_duration = 320;
    QEasingCurve m_easing{QEasingCurve::InOutCubic};
    QElapsedTimer m_clock;
    qreal m_progress = 1.0;
    QTimer *m_timer = nullptr;
};

// A widget showing a MorphIcon. The color follows the palette's WindowText
// (or setColor()). sizeHint() is iconSize + padding.
class SHADE_SDK_EXPORT IconWidget : public QWidget
{
    Q_OBJECT

public:
    explicit IconWidget(const QString &source = {}, int iconSize = 16, QWidget *parent = nullptr);

    MorphIcon *icon() const { return m_icon; }
    void setSource(const QString &source, bool animate = true) { m_icon->setSource(source, animate); }
    void setColor(const QColor &color);
    QColor color() const;
    void setIconSize(int size);
    int iconSize() const { return m_iconSize; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *event) override;

    MorphIcon *m_icon;
    int m_iconSize;
    QColor m_color;
};

// A flat, round, clickable icon with an animated hover highlight.
class SHADE_SDK_EXPORT IconButton : public IconWidget
{
    Q_OBJECT

public:
    explicit IconButton(const QString &source = {}, int iconSize = 16, QWidget *parent = nullptr);

    QSize sizeHint() const override;
    void setPadding(int padding) { m_padding = padding; updateGeometry(); }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void animateHover(qreal to);

    int m_padding = 6;
    qreal m_hover = 0.0;
    qreal m_press = 0.0;
    bool m_pressed = false;
    QTimer *m_hoverTimer = nullptr;
    qreal m_hoverTarget = 0.0;
};

} // namespace shade
