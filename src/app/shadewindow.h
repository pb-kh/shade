#pragma once

#include "core/model.h"

#include <shade/pane.h>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QImage>
#include <QPointer>
#include <QRegion>
#include <QWidget>

#include <memory>
#include <vector>

class QScreen;
class QTimer;
class QWheelEvent;
class QKeyEvent;
class QPainter;

namespace LayerShellQt
{
class Window;
}

namespace shade::term
{
class Terminal;
}

namespace shade::app
{

// One overlay window per monitor.
//
// Rendering is hybrid: while anything moves (expand/collapse, paging, peek
// morphs) the window composites snapshots of the plugin widgets into the
// pill, clipped to its rounded shape with antialiasing, with crossfades and
// the peek->widget morph. Once the shade rests on a pane, that pane's real
// widget is shown live at exactly the same place, so Qt delivers input,
// hover, tooltips and cursors normally.
class ShadeWindow : public QWidget
{
    Q_OBJECT

public:
    ShadeWindow(QScreen *screen, const core::Settings *settings, QWidget *parent = nullptr);
    ~ShadeWindow() override;

    QString name() const { return m_name; }
    QScreen *targetScreen() const { return m_screen; }
    core::ShadeModel *model() const { return m_model; }

    void showOnLayer();

    // Pane widgets are owned by the window once added.
    void addPane(const QString &plugin, int order, shade::PaneWidget *widget);
    void removePane(const QString &plugin);
    shade::PaneWidget *pane(const QString &plugin) const;
    QStringList paneNames() const;
    void setPlaceholder(shade::PaneWidget *widget);

    QSizeF peekSize(const QString &plugin, const shade::PeekView &view) const;

    bool isLive() const { return m_live; }
    // Render the current frame (what the user sees) into an image.
    QImage renderFrame();
    QRegion inputRegion() const { return m_inputRegion; }

    // Debug / test injection (control commands).
    void injectPointer(const QPointF &pos);
    void injectLeave();
    void injectClick(const QPointF &pos);
    void injectWheel(qreal dx, qreal dy, bool end, bool touchpad = false);

    // Embedded terminals (terminal_request): at most one per owner, shown
    // below the pill inside this same surface. They grow out of the pill
    // with the expand bounce and retract into it on collapse, owner
    // deactivation, close, or when the command exits.
    bool openTerminal(const QString &owner, const QStringList &argv, const QSize &size, QString *error);
    bool closeTerminal(const QString &owner, const QString &reason); // animated
    void removeTerminals(const QString &owner);                      // immediate (unload)
    bool hasTerminal(const QString &owner) const;
    QJsonArray terminalsJson(bool withText) const;
    bool typeIntoTerminal(const QString &owner, const QString &text);
    void retractAllTerminals(const QString &reason);

signals:
    void activePluginChanged(const QString &plugin);
    void peekRemoved(const QString &plugin, const QString &id, const QString &reason);
    void terminalClosed(const QString &owner, const QString &reason, int exitCode);
    void pointerEntered();

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Pane
    {
        QString plugin;
        int order = 0;
        QPointer<shade::PaneWidget> widget;
        bool placeholder = false;
    };

    void wake();
    void frame();
    void syncPanes();
    void updateLive();
    void updateInputRegion();
    void updateCanvasSize();
    void placeOnScreen();
    void paintScene(QPainter &painter, bool includeLive);
    QImage snapshot(shade::PaneWidget *w) const;
    shade::PaneWidget *peekOwner(const core::PeekEntry *entry) const;
    void peekPointer(QEvent::Type type, const QPointF &pos, Qt::MouseButton button);
    void handleMove(const QPointF &pos);
    QPointF wheelDelta(const QWheelEvent *e) const;

    // Terminals (terminals.cpp, or terminals_stub.cpp without libvterm).
    struct TermSession;
    bool stepTerminals(qreal dt);
    void paintTerminals(QPainter &p);
    bool terminalPointer(QEvent::Type type, const QPointF &pos, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    bool terminalWheel(const QPointF &pos, const QPointF &delta, Qt::KeyboardModifiers mods);
    bool terminalKey(QKeyEvent *e);
    void terminalsFocusChanged(bool active);
    QRectF terminalsBounds() const;   // input region needed by terminals
    QSize terminalsCanvasNeed() const; // surface size needed by terminals
    void destroyTerminals();
    QRectF terminalFinalRect(const TermSession &t) const;
    QRectF terminalRect(const TermSession &t) const;
    QPointF terminalOrigin(const TermSession &t) const;
    TermSession *terminalAt(const QPointF &pos);
    TermSession *session(const QString &owner) const;
    void retractTerminal(TermSession &t, const QString &reason);
    void dropTerminal(TermSession *t, const QString &reason, bool notify);
    void focusTerminal(TermSession *t);
    void updateKeyboard();
    QSize clampTerminal(QSize size) const;
    qreal maxPaneHeight() const;

    const core::Settings *m_s;
    QScreen *m_screen;
    QString m_name;
    core::ShadeModel *m_model;
    LayerShellQt::Window *m_layer = nullptr;
    QTimer *m_timer;
    QTimer *m_poll;
    QElapsedTimer m_clock;
    QVector<Pane> m_panes;
    QPointer<shade::PaneWidget> m_placeholder;
    bool m_live = false;
    QPointer<shade::PaneWidget> m_liveWidget;
    QRegion m_inputRegion;
    QSize m_canvas;

    std::vector<TermSession *> m_terms; // owned (terminals.cpp)
    QString m_termFocus;   // owner of the keyboard-focused terminal
    QString m_termGrab;    // owner receiving a pressed mouse button
    qreal m_termWheel = 0; // pixel accumulator for terminal wheel steps
    bool m_keyboardWanted = false;
};

} // namespace shade::app
