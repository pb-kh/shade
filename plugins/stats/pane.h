#pragma once

#include <shade/icon.h>
#include <shade/pane.h>

#include <QHash>
#include <QPointer>

class QLabel;
class QTimer;

namespace stats
{

class StatsPlugin;

// One indicator: an icon, an optional value, and a detail text that slides
// out on hover (e.g. the SSID next to the Wi-Fi icon).
class StatItem : public QWidget
{
    Q_OBJECT

public:
    explicit StatItem(const QString &name, QWidget *parent = nullptr);

    void setIcon(const QString &source);
    void setValue(const QString &value);
    void setDetail(const QString &detail);
    void setDimmed(bool dimmed);
    void setInteractive(bool interactive);

    QString value() const { return m_value; }
    QString detail() const { return m_detail; }
    QString iconSource() const { return m_icon->source(); }
    qreal reveal() const { return m_reveal; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void clicked(Qt::MouseButton button);
    void scrolled(int steps);

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    void animate();

    shade::MorphIcon *m_icon;
    QString m_value;
    QString m_detail;
    bool m_dimmed = false;
    bool m_interactive = true;
    bool m_hovered = false;
    qreal m_hover = 0; // highlight 0..1
    qreal m_reveal = 0; // detail width 0..1
    qreal m_revealVel = 0;
    QTimer *m_anim;
    qreal m_wheelAccum = 0;
};

class StatsPane : public shade::PaneWidget
{
    Q_OBJECT

public:
    StatsPane(StatsPlugin *plugin, const QString &window, QWidget *parent = nullptr);
    QString windowName() const { return m_window; }

    QSize sizeHint() const override;
    QSize peekSizeHint(const shade::PeekView &peek) const override;
    void paintPeek(QPainter *painter, const QRectF &rect, const shade::PeekView &peek, qreal morph) override;

    StatItem *item(const QString &name) const;
    QString timeText() const;

public slots:
    void refresh();
    void tick();

private:
    shade::MorphIcon *peekIcon(const QString &id, const QString &source);
    qreal peekLevel(const QString &id, qreal target);

    QPointer<StatsPlugin> m_plugin;
    QString m_window; // monitor this pane lives on: tools open there
    QLabel *m_time;
    QHash<QString, StatItem *> m_items;
    QHash<QString, shade::MorphIcon *> m_peekIcons;
    QHash<QString, qreal> m_peekLevels;
    QHash<QString, qreal> m_peekTargets;
    QTimer *m_levelAnim;
};

} // namespace stats
