#pragma once

#include <shade/icon.h>
#include <shade/pane.h>

#include <QImage>
#include <QPointer>

class QTimer;

namespace media
{

class MediaPlugin;

class MediaPane : public shade::PaneWidget
{
    Q_OBJECT

public:
    explicit MediaPane(MediaPlugin *plugin, QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize peekSizeHint(const shade::PeekView &peek) const override;
    void paintPeek(QPainter *painter, const QRectF &rect, const shade::PeekView &peek, qreal morph) override;

    // Geometry (pane coordinates), exposed for tests.
    QRectF artRect() const { return QRectF(14, 14, 64, 64); }
    QRectF seekRect() const;
    QString elapsedText() const;
    QString totalText() const;

public slots:
    void refresh();
    void setArt(const QImage &art);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    void layoutButtons();
    void paintArt(QPainter *p, const QRectF &r, qreal radius, qreal fade) const;
    qint64 seekAt(qreal x) const;
    static QString formatTime(qint64 us, bool hours);

    QPointer<MediaPlugin> m_plugin;
    shade::IconButton *m_shuffle;
    shade::IconButton *m_prev;
    shade::IconButton *m_play;
    shade::IconButton *m_next;
    shade::IconButton *m_loop;
    shade::MorphIcon *m_note;
    QImage m_art, m_artPrev;
    QImage m_bg, m_bgPrev;
    qreal m_artFade = 1.0;
    QTimer *m_fade;
    QTimer *m_progress;
    bool m_seeking = false;
    qint64 m_seekPreview = -1;
};

} // namespace media
