#include "shadewindow.h"

#include <shade/theme.h>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>

#ifdef SHADE_HAVE_LAYER_SHELL
#include <LayerShellQt/Window>
#endif

#include <algorithm>
#include <cmath>

namespace shade::app
{

namespace
{
QPainterPath roundedPill(const QRectF &r, qreal radius)
{
    QPainterPath p;
    p.addRoundedRect(r, radius, radius);
    return p;
}

QPointF aligned(const QPointF &p) { return QPointF(std::round(p.x()), std::round(p.y())); }

bool fromTouchpad(const QWheelEvent *e)
{
    const QPointingDevice *dev = e->pointingDevice();
    if (dev && dev->type() == QInputDevice::DeviceType::TouchPad) return true;
    // Some backends do not tag the device: only touchpads send pixel deltas
    // together with scroll phases.
    return !e->pixelDelta().isNull() && e->phase() != Qt::NoScrollPhase;
}
} // namespace

// Wheel delta in pixels, with touchpad swipes' horizontal direction
// reversed (touchpad_scroll_direction) relative to wheels.
QPointF ShadeWindow::wheelDelta(const QWheelEvent *e) const
{
    QPointF d = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 120.0 * m_s->pixelsPerWheelNotch : QPointF(e->pixelDelta());
    if (fromTouchpad(e)) d.setX(d.x() * m_s->touchpadScrollDirection);
    return d;
}

ShadeWindow::ShadeWindow(QScreen *screen, const core::Settings *settings, QWidget *parent)
    : QWidget(parent)
    , m_s(settings)
    , m_screen(screen)
    , m_name(screen && !screen->name().isEmpty() ? screen->name() : QStringLiteral("default"))
    , m_model(new core::ShadeModel(settings, this))
{
    setObjectName(QStringLiteral("shade-") + m_name);
    setWindowTitle(QStringLiteral("shade"));
    // Accepts focus: embedded terminals take the keyboard (layer-shell keyboard
    // interactivity stays "none" unless a terminal is presented).
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);

    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, &ShadeWindow::frame);

    // Plugins may change their size without telling us; a cheap poll keeps
    // the pill matched to sizeHint().
    m_poll = new QTimer(this);
    m_poll->setInterval(300);
    connect(m_poll, &QTimer::timeout, this, &ShadeWindow::syncPanes);
    m_poll->start();

    connect(m_model, &core::ShadeModel::wakeUp, this, &ShadeWindow::wake);
    connect(m_model, &core::ShadeModel::activePluginChanged, this, [this](const QString &plugin) {
        for (const Pane &p : m_panes) {
            if (p.widget) p.widget->hostSetActive(p.plugin == plugin && !plugin.isEmpty());
        }
        // A terminal belongs to its plugin's pane: paging away retracts it.
        for (const Pane &p : m_panes) {
            if (p.plugin != plugin && hasTerminal(p.plugin)) closeTerminal(p.plugin, QStringLiteral("deactivated"));
        }
        emit activePluginChanged(plugin);
    });
    connect(m_model, &core::ShadeModel::peekRemoved, this, &ShadeWindow::peekRemoved);
    connect(m_model, &core::ShadeModel::stateChanged, this, [this]() {
        if (m_model->state() != core::ShadeModel::State::Widgets) retractAllTerminals(QStringLiteral("collapsed"));
        updateLive();
        updateInputRegion();
    });

    updateCanvasSize();
}

ShadeWindow::~ShadeWindow()
{
    destroyTerminals();
    for (Pane &p : m_panes) delete p.widget.data();
}

void ShadeWindow::showOnLayer()
{
    const bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    if (m_screen) setScreen(m_screen);
#ifdef SHADE_HAVE_LAYER_SHELL
    if (wayland) {
        createWinId();
        if (QWindow *w = windowHandle()) {
            m_layer = LayerShellQt::Window::get(w);
            if (m_layer) {
                using LW = LayerShellQt::Window;
                // Anchored to the top edge only: the compositor centers us.
                m_layer->setAnchors(LW::Anchors(LW::AnchorTop));
                m_layer->setLayer(LW::LayerOverlay);
                m_layer->setKeyboardInteractivity(LW::KeyboardInteractivityNone);
                m_layer->setExclusiveZone(-1);
                m_layer->setScope(QStringLiteral("shade"));
                m_layer->setActivateOnShow(false);
                if (m_screen) m_layer->setScreen(m_screen);
            }
        }
    }
#else
    Q_UNUSED(wayland);
#endif
    if (!m_layer) setWindowFlag(Qt::WindowStaysOnTopHint, true);
    placeOnScreen();
    show();
    updateInputRegion();
    if (m_screen) {
        connect(m_screen, &QScreen::geometryChanged, this, &ShadeWindow::placeOnScreen);
    }
    wake();
}

void ShadeWindow::placeOnScreen()
{
    if (m_layer || !m_screen) return;
    const QRect g = m_screen->geometry();
    move(g.x() + (g.width() - width()) / 2, g.y());
}

void ShadeWindow::updateCanvasSize()
{
    qreal w = std::max<qreal>(m_s->barWidth, 440);
    qreal h = 90;
    for (const Pane &p : m_panes) {
        if (!p.widget) continue;
        const QSize s = p.widget->sizeHint();
        w = std::max<qreal>(w, s.width());
        h = std::max<qreal>(h, s.height());
    }
    // Room for the bounce overshoot and the rubber band.
    QSize canvas(int(std::ceil(w * 1.2 + 2 * m_s->overscroll + 32)), int(std::ceil(m_s->topOffset + h * 1.25 + 32)));
    canvas = canvas.expandedTo(terminalsCanvasNeed());
    if (m_screen) canvas = canvas.boundedTo(m_screen->geometry().size());
    // Even width keeps the pill center on a pixel boundary.
    canvas.setWidth(canvas.width() & ~1);
    if (canvas == m_canvas) return;
    // Grow eagerly, shrink only when it is a lot smaller (avoids surface
    // resizes while a list grows and shrinks).
    if (!m_canvas.isEmpty() && canvas.width() <= m_canvas.width() && canvas.height() <= m_canvas.height() &&
        canvas.width() > m_canvas.width() * 0.7 && canvas.height() > m_canvas.height() * 0.7) {
        return;
    }
    m_canvas = canvas;
    setFixedSize(m_canvas);
    m_model->setCanvasWidth(m_canvas.width());
    placeOnScreen();
    updateInputRegion();
}

void ShadeWindow::addPane(const QString &plugin, int order, shade::PaneWidget *widget)
{
    if (!widget) return;
    removePane(plugin);
    widget->setParent(this);
    widget->hide();
    widget->hostSetCornerRadius(m_s->cornerRadius);
    widget->installEventFilter(this);
    connect(widget, &shade::PaneWidget::paneSizeChanged, this, &ShadeWindow::syncPanes);
    connect(widget, &shade::PaneWidget::peekRepaintRequested, this, [this]() {
        if (m_model->state() != core::ShadeModel::State::Widgets || m_model->morph() < 0.999) update();
    });
    connect(widget, &shade::PaneWidget::peekGeometryChanged, this, [this, plugin]() {
        for (const core::PeekEntry &e : m_model->queue().entries()) {
            if (e.plugin == plugin) m_model->updatePeekSize(plugin, e.id, peekSize(plugin, {e.id, e.data}));
        }
    });
    connect(widget, &QObject::destroyed, this, [this, plugin]() {
        m_panes.removeIf([&](const Pane &p) { return p.plugin == plugin && !p.widget; });
        syncPanes();
    });
    Pane p;
    p.plugin = plugin;
    p.order = order;
    p.widget = widget;
    m_panes.push_back(p);
    std::stable_sort(m_panes.begin(), m_panes.end(), [](const Pane &a, const Pane &b) { return a.order < b.order; });
    syncPanes();
}

void ShadeWindow::removePane(const QString &plugin)
{
    for (int i = 0; i < m_panes.size(); ++i) {
        if (m_panes[i].plugin == plugin) {
            shade::PaneWidget *w = m_panes[i].widget;
            m_panes.removeAt(i);
            if (w == m_liveWidget) {
                m_live = false;
                m_liveWidget = nullptr;
            }
            m_model->dismissPeek(plugin, QString(), QStringLiteral("unload"));
            delete w;
            syncPanes();
            return;
        }
    }
}

void ShadeWindow::setPlaceholder(shade::PaneWidget *widget)
{
    m_placeholder = widget;
    if (widget) {
        widget->setParent(this);
        widget->hide();
        widget->hostSetCornerRadius(m_s->cornerRadius);
    }
    syncPanes();
}

shade::PaneWidget *ShadeWindow::pane(const QString &plugin) const
{
    for (const Pane &p : m_panes) {
        if (p.plugin == plugin) return p.widget;
    }
    if (plugin == QLatin1String("placeholder")) return m_placeholder;
    return nullptr;
}

QStringList ShadeWindow::paneNames() const
{
    QStringList out;
    for (const Pane &p : m_panes) {
        if (p.widget) out << p.plugin;
    }
    return out;
}

void ShadeWindow::syncPanes()
{
    QVector<core::PaneInfo> infos;
    for (const Pane &p : m_panes) {
        if (!p.widget) continue;
        const QSize s = p.widget->sizeHint().expandedTo(QSize(1, 1));
        if (p.widget->size() != s) p.widget->resize(s);
        infos.push_back({p.plugin, QSizeF(s)});
    }
    if (infos.isEmpty() && m_placeholder) {
        const QSize s = m_placeholder->sizeHint().expandedTo(QSize(1, 1));
        m_placeholder->resize(s);
        infos.push_back({QStringLiteral("placeholder"), QSizeF(s)});
    }
    bool changed = infos.size() != m_model->panes().size();
    for (int i = 0; !changed && i < infos.size(); ++i) {
        changed = infos[i].plugin != m_model->panes()[i].plugin || infos[i].size != m_model->panes()[i].size;
    }
    if (!changed) return;
    m_model->setPanes(infos);
    updateCanvasSize();
    if (m_live && m_liveWidget) {
        // Size changes while live: follow the pill (composited until settled).
        updateLive();
    }
    wake();
}

QSizeF ShadeWindow::peekSize(const QString &plugin, const shade::PeekView &view) const
{
    shade::PaneWidget *w = pane(plugin);
    if (!w) return {};
    QSize s = w->peekSizeHint(view);
    s = s.boundedTo(QSize(int(m_canvas.width() - 2 * m_s->overscroll - 16), int(m_canvas.height() - m_s->topOffset - 16)));
    return QSizeF(s.expandedTo(QSize(24, 16)));
}

shade::PaneWidget *ShadeWindow::peekOwner(const core::PeekEntry *entry) const
{
    return entry ? pane(entry->plugin) : nullptr;
}

void ShadeWindow::wake()
{
    if (!m_clock.isValid()) m_clock.start();
    if (!m_timer->isActive()) {
        m_clock.restart();
        m_timer->setInterval(16);
        m_timer->start();
    }
    update();
}

void ShadeWindow::frame()
{
    const qreal dt = m_clock.isValid() ? m_clock.restart() / 1000.0 : 1.0 / 60;
    bool more = m_model->tick(dt);
    more |= stepTerminals(dt);

    // Owner of a morphing peek sees the morph value.
    const QString sel = m_model->selectedPlugin();
    for (const Pane &p : m_panes) {
        if (p.widget) p.widget->hostSetMorph(p.plugin == sel ? m_model->morph() : 1.0);
    }

    updateLive();
    updateInputRegion();
    update();

    if (!more) {
        m_timer->stop();
        update();
    } else if (m_model->geometrySettled() && m_terms.empty()) {
        // Only peek timers are running: no need for 60 fps.
        m_timer->setInterval(100);
    } else {
        m_timer->setInterval(16);
    }
}

void ShadeWindow::updateLive()
{
    shade::PaneWidget *want = nullptr;
    if (m_model->restingOnPane()) {
        const int i = m_model->selected();
        if (i >= 0 && i < m_model->panes().size()) want = pane(m_model->panes()[i].plugin);
        // Going live needs the pill to match the pane; staying live is fine
        // while the pane's own size change is animating (e.g. a list grew).
        if (want && want != m_liveWidget && m_model->pillRect().size() != QSizeF(want->size())) want = nullptr;
    }
    if (m_liveWidget && m_liveWidget != want) {
        m_liveWidget->clearMask();
        m_liveWidget->hide();
    }
    const bool changed = want != m_liveWidget;
    m_liveWidget = want;
    m_live = want != nullptr;
    if (!want) {
        if (changed) update();
        return;
    }
    const QRect r(aligned(m_model->paneRect(m_model->selected()).topLeft()).toPoint(), want->size());
    if (want->geometry() != r) want->setGeometry(r);
    // While the pill is still catching up with a resized pane, clip the live
    // widget to the pill.
    const QRect pill = m_model->pillRect().toAlignedRect();
    if (pill.contains(r)) {
        if (!want->mask().isEmpty()) want->clearMask();
    } else {
        QPainterPath path = roundedPill(m_model->pillRect(), m_model->radius());
        path.translate(-QPointF(r.topLeft()));
        want->setMask(QRegion(path.toFillPolygon().toPolygon()));
    }
    if (changed) {
        want->show();
        want->raise();
        update();
    }
}

void ShadeWindow::updateInputRegion()
{
    const QRectF pill = m_model->pillRect();
    QRegion region;
    switch (m_model->state()) {
    case core::ShadeModel::State::Bar: {
        const qreal w = m_s->barWidth + 16;
        region = QRegion(QRectF(m_canvas.width() / 2.0 - w / 2.0, 0, w, std::max(m_s->hoverZone, pill.bottom() + 2)).toAlignedRect());
        break;
    }
    case core::ShadeModel::State::Peek:
    case core::ShadeModel::State::Widgets: {
        // The pill plus the strip above it, so reaching for the screen top
        // never "leaves" the shade.
        const QRectF r(pill.left() - 2, 0, pill.width() + 4, pill.bottom() + 3);
        region = QRegion(r.toAlignedRect());
        break;
    }
    }
    const QRectF terms = terminalsBounds();
    if (!terms.isEmpty()) region |= QRegion(terms.toAlignedRect().adjusted(-2, -2, 2, 2));
    region &= QRegion(rect());
    if (region == m_inputRegion) return;
    // The offscreen platform (tests) has no input regions.
    static const bool masks = QGuiApplication::platformName() != QLatin1String("offscreen");
    if (masks && !windowHandle()) return; // not created yet: apply on a later frame
    m_inputRegion = region;
    // QWindow::setMask, not QWidget::setMask: on Wayland the window mask is
    // only the input region, while a widget mask also clips painting. A
    // clipped paint never clears pixels outside the (shrinking) mask, so the
    // compositor kept showing stale frames as trails.
    if (masks && windowHandle()) windowHandle()->setMask(region);
}

QImage ShadeWindow::snapshot(shade::PaneWidget *w) const
{
    const qreal dpr = devicePixelRatioF();
    QImage img(w->size() * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    w->render(&img, QPoint(), QRegion(), QWidget::DrawChildren);
    return img;
}

void ShadeWindow::paintScene(QPainter &p, bool includeLive)
{
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF pill = m_model->pillRect();
    if (pill.width() < 0.5 || pill.height() < 0.5) return;
    const qreal radius = m_model->radius();
    const QPainterPath shape = roundedPill(pill, radius);
    const Theme &theme = Theme::get();

    const qreal open = m_model->contentOpacity();
    p.fillPath(shape, mix(theme.color(QStringLiteral("bar")), theme.bg(), open));
    if (open <= 0.001) return;

    const bool liveNow = m_live && m_liveWidget && !includeLive;
    if (liveNow) return; // the live widget draws itself on top

    const qreal dpr = devicePixelRatioF();
    const QRect bounds = pill.toAlignedRect();
    QImage layer(bounds.size() * dpr, QImage::Format_ARGB32_Premultiplied);
    layer.setDevicePixelRatio(dpr);
    layer.fill(Qt::transparent);
    {
        QPainter lp(&layer);
        lp.setRenderHint(QPainter::Antialiasing);
        lp.setRenderHint(QPainter::SmoothPixmapTransform);
        lp.translate(-bounds.topLeft());
        const qreal morph = m_model->morph();

        if (morph > 0.001) {
            for (int i = 0; i < m_model->panes().size(); ++i) {
                const QRectF r = m_model->paneRect(i);
                if (!r.intersects(pill)) continue;
                shade::PaneWidget *w = pane(m_model->panes()[i].plugin);
                if (!w) continue;
                lp.setOpacity(morph);
                lp.drawImage(aligned(r.topLeft()), snapshot(w));
            }
        }
        if (morph < 0.999) {
            const core::PeekEntry *front = m_model->presentedPeek();
            const core::PeekEntry *out = m_model->outgoingPeek();
            const qreal swap = out ? m_model->outgoingOpacity() : 0.0;
            if (out) {
                if (shade::PaneWidget *ow = peekOwner(out)) {
                    lp.save();
                    lp.setOpacity((1 - morph) * swap);
                    ow->paintPeek(&lp, pill, {out->id, out->data}, morph);
                    lp.restore();
                }
            }
            if (front) {
                if (shade::PaneWidget *fw = peekOwner(front)) {
                    lp.save();
                    lp.setOpacity((1 - morph) * (1 - swap));
                    fw->paintPeek(&lp, pill, {front->id, front->data}, morph);
                    lp.restore();
                }
            }
        }
        // Antialiased rounded clip.
        lp.setOpacity(1.0);
        lp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        lp.fillPath(shape, Qt::black);
    }
    p.setOpacity(open);
    p.drawImage(bounds.topLeft(), layer);
    p.setOpacity(1.0);
}

void ShadeWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    paintScene(p, false);
    paintTerminals(p);
}

QImage ShadeWindow::renderFrame()
{
    QImage img(size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    paintScene(p, true);
    paintTerminals(p);
    if (m_live && m_liveWidget) {
        p.end();
        // The live widget, rendered where it sits.
        QPainter q(&img);
        q.drawImage(m_liveWidget->pos(), snapshot(m_liveWidget));
    }
    return img;
}

// --- input -------------------------------------------------------------------

void ShadeWindow::handleMove(const QPointF &pos)
{
    m_model->pointerMoved(pos);
    if (m_model->state() == core::ShadeModel::State::Peek) {
        const QRectF pill = m_model->pillRect();
        if (pill.contains(pos)) peekPointer(QEvent::MouseMove, pos, Qt::NoButton);
    }
}

void ShadeWindow::enterEvent(QEnterEvent *e)
{
    emit pointerEntered();
    handleMove(e->position());
}

void ShadeWindow::leaveEvent(QEvent *)
{
    // Qt only sends Leave to the window when the pointer leaves it entirely
    // (moving into a live child widget is not a leave).
    m_model->pointerLeft();
}

void ShadeWindow::mouseMoveEvent(QMouseEvent *e)
{
    if (terminalPointer(QEvent::MouseMove, e->position(), Qt::NoButton, e->modifiers())) {
        m_model->pointerMoved(e->position()); // keeps the idle timer honest
        return;
    }
    handleMove(e->position());
}

void ShadeWindow::peekPointer(QEvent::Type type, const QPointF &pos, Qt::MouseButton button)
{
    const core::PeekEntry *front = m_model->presentedPeek();
    shade::PaneWidget *w = peekOwner(front);
    if (!w) return;
    const QRectF pill = m_model->pillRect();
    if (w->peekPointerEvent(type, pos - pill.topLeft(), button, {front->id, front->data})) update();
}

void ShadeWindow::mousePressEvent(QMouseEvent *e)
{
    if (terminalPointer(QEvent::MouseButtonPress, e->position(), e->button(), e->modifiers())) return;
    if (m_model->state() == core::ShadeModel::State::Bar) {
        m_model->expand(); // poke the bar
        return;
    }
    if (m_model->state() == core::ShadeModel::State::Peek && m_model->pillRect().contains(e->position())) {
        peekPointer(QEvent::MouseButtonPress, e->position(), e->button());
    }
}

void ShadeWindow::mouseReleaseEvent(QMouseEvent *e)
{
    if (terminalPointer(QEvent::MouseButtonRelease, e->position(), e->button(), e->modifiers())) return;
    if (m_model->state() == core::ShadeModel::State::Peek && m_model->pillRect().contains(e->position())) {
        peekPointer(QEvent::MouseButtonRelease, e->position(), e->button());
    }
}

void ShadeWindow::wheelEvent(QWheelEvent *e)
{
    if (e->phase() == Qt::ScrollEnd || e->phase() == Qt::ScrollMomentum) {
        m_model->wheel(0, 0, true);
        e->accept();
        return;
    }
    const QPointF d = wheelDelta(e);
    const qreal dx = d.x(), dy = d.y();
    if (std::abs(dy) >= std::abs(dx) && terminalWheel(e->position(), d, e->modifiers())) {
        e->accept();
        return;
    }
    // A positive horizontal delta advances (flip with scroll_direction).
    if (m_model->wheel(dx, dy)) {
        e->accept();
        return;
    }
    // Vertical wheel over a pane while composited: forward to the widget.
    if (m_model->state() == core::ShadeModel::State::Widgets && !m_live) {
        const int i = m_model->selected();
        if (shade::PaneWidget *w = i >= 0 && i < m_model->panes().size() ? pane(m_model->panes()[i].plugin) : nullptr) {
            const QPointF local = e->position() - aligned(m_model->paneRect(i).topLeft());
            QWidget *target = w->childAt(local.toPoint());
            if (!target) target = w;
            QWheelEvent fwd(target->mapFrom(w, local), e->globalPosition(), e->pixelDelta(), e->angleDelta(),
                            e->buttons(), e->modifiers(), e->phase(), e->inverted());
            QCoreApplication::sendEvent(target, &fwd);
        }
    }
    e->accept();
}

void ShadeWindow::keyPressEvent(QKeyEvent *e)
{
    if (terminalKey(e)) {
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

bool ShadeWindow::event(QEvent *e)
{
    switch (e->type()) {
    case QEvent::ShortcutOverride:
    case QEvent::KeyPress:
        // Tab, Ctrl+..., Esc belong to the terminal, not to Qt's focus chain
        // or shortcuts.
        if (!m_termFocus.isEmpty()) {
            if (e->type() == QEvent::KeyPress) keyPressEvent(static_cast<QKeyEvent *>(e));
            e->accept();
            return true;
        }
        break;
    case QEvent::WindowActivate: terminalsFocusChanged(true); break;
    case QEvent::WindowDeactivate: terminalsFocusChanged(false); break;
    default: break;
    }
    return QWidget::event(e);
}

bool ShadeWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::LayoutRequest) {
        QTimer::singleShot(0, this, &ShadeWindow::syncPanes);
    }
    // Horizontal wheel over a live pane that the pane did not consume: page.
    if (event->type() == QEvent::Wheel && watched == m_liveWidget) {
        auto *we = static_cast<QWheelEvent *>(event);
        const QPointF d = wheelDelta(we);
        if (std::abs(d.x()) > std::abs(d.y())) {
            m_model->wheel(d.x(), d.y());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ShadeWindow::injectPointer(const QPointF &pos)
{
    if (m_live && m_liveWidget && m_liveWidget->geometry().contains(pos.toPoint())) {
        QWidget *target = m_liveWidget->childAt(m_liveWidget->mapFrom(this, pos.toPoint()));
        if (!target) target = m_liveWidget;
        const QPointF local = target->mapFrom(this, pos);
        QMouseEvent me(QEvent::MouseMove, local, mapToGlobal(pos), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &me);
        return;
    }
    handleMove(pos);
}

void ShadeWindow::injectLeave() { m_model->pointerLeft(); }

void ShadeWindow::injectClick(const QPointF &pos)
{
    if (m_live && m_liveWidget && m_liveWidget->geometry().contains(pos.toPoint())) {
        QWidget *target = m_liveWidget->childAt(m_liveWidget->mapFrom(this, pos.toPoint()));
        if (!target) target = m_liveWidget;
        const QPointF local = target->mapFrom(this, pos);
        QEnterEvent enter(local, local, mapToGlobal(pos));
        QCoreApplication::sendEvent(target, &enter);
        for (QEvent::Type t : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent me(t, local, mapToGlobal(pos), Qt::LeftButton,
                           t == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(target, &me);
        }
        return;
    }
    for (QEvent::Type t : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent me(t, pos, mapToGlobal(pos), Qt::LeftButton,
                       t == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        if (t == QEvent::MouseButtonPress) mousePressEvent(&me);
        else mouseReleaseEvent(&me);
    }
}

void ShadeWindow::injectWheel(qreal dx, qreal dy, bool end, bool touchpad)
{
    if (end) {
        m_model->wheel(0, 0, true);
        return;
    }
    m_model->wheel(touchpad ? dx * m_s->touchpadScrollDirection : dx, dy);
}

} // namespace shade::app
