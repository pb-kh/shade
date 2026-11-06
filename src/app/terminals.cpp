// Embedded terminals of a ShadeWindow (terminal_request).
//
// A terminal lives in the shade's own layer surface, below the pill: one
// surface means no cross-surface stacking problems, and moving the pointer
// from the pill into the terminal is not a "leave", so the shade never
// collapses while the terminal is being used. The surface is resized in
// discrete steps (updateCanvasSize) when a terminal appears or goes away,
// never per frame; the motion itself is a clipped viewport driven by four
// springs (center x, top, width, height).
#include "shadewindow.h"

#include "term/terminal.h"

#include <shade/theme.h>

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonObject>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWindow>

#ifdef SHADE_HAVE_LAYER_SHELL
#include <LayerShellQt/Window>
#endif

#include <cmath>

namespace shade::app
{

struct ShadeWindow::TermSession
{
    QString owner;
    QString command;
    std::unique_ptr<term::Terminal> term;
    QSize size;               // requested, clamped (pixels, including padding)
    core::Spring cx, top, w, h; // the viewport
    bool retracting = false;
    QString reason;
    int exitCode = -1;
};

namespace
{
constexpr int kMinTerminal = 80;
constexpr int kMaxTerminal = 8192;

// A terminal that is still running when its session goes away is
// terminated asynchronously; the object deletes itself once reaped.
void retire(std::unique_ptr<term::Terminal> t)
{
    if (!t) return;
    term::Terminal *raw = t.release();
    raw->disconnect();
    if (!raw->running()) {
        raw->deleteLater();
        return;
    }
    QObject::connect(raw, &term::Terminal::finished, raw, &QObject::deleteLater);
    raw->terminate();
}
} // namespace

qreal ShadeWindow::maxPaneHeight() const
{
    qreal h = 0;
    for (const Pane &p : m_panes) {
        if (p.widget) h = std::max<qreal>(h, p.widget->sizeHint().height());
    }
    return std::max<qreal>(h, 44);
}

QSize ShadeWindow::clampTerminal(QSize size) const
{
    // Keep the terminal within the monitor below the tallest pane.
    QSize screen = m_screen ? m_screen->geometry().size() : QSize(1920, 1080);
    const int maxW = std::max(kMinTerminal, screen.width() - 32);
    const int maxH = std::max(kMinTerminal, int(screen.height() - m_s->topOffset - maxPaneHeight() * 1.1 - m_s->terminalGap - 32));
    return QSize(std::clamp(size.width(), kMinTerminal, maxW), std::clamp(size.height(), kMinTerminal, maxH));
}

ShadeWindow::TermSession *ShadeWindow::session(const QString &owner) const
{
    for (TermSession *t : m_terms) {
        if (t->owner == owner) return t;
    }
    return nullptr;
}

bool ShadeWindow::hasTerminal(const QString &owner) const
{
    const TermSession *t = session(owner);
    return t && !t->retracting;
}

bool ShadeWindow::openTerminal(const QString &owner, const QStringList &argv, const QSize &requested, QString *error)
{
    if (argv.isEmpty() || argv.first().isEmpty()) {
        if (error) *error = QStringLiteral("empty command");
        return false;
    }
    if (requested.width() < kMinTerminal || requested.height() < kMinTerminal || requested.width() > kMaxTerminal ||
        requested.height() > kMaxTerminal) {
        if (error) *error = QStringLiteral("size %1x%2 outside [%3, %4]").arg(requested.width()).arg(requested.height()).arg(kMinTerminal).arg(kMaxTerminal);
        return false;
    }
    // The terminal hangs below the open shade: show the owner's pane first.
    if (m_model->state() != core::ShadeModel::State::Widgets) {
        if (!pane(owner) || !m_model->focusPlugin(owner)) m_model->expand();
    }
    const QSize size = clampTerminal(requested);
    QFont font(m_s->terminalFont);
    font.setPointSizeF(m_s->terminalFontSize);
    auto t = std::make_unique<term::Terminal>(font, term::themePalette(), m_s->terminalScrollback);
    const qreal pad = m_s->terminalPadding;
    const QSize inner(int(size.width() - 2 * pad), int(size.height() - 2 * pad));
    if (!t->start(argv, inner, error)) return false;

    TermSession *s = session(owner);
    const bool replacing = s != nullptr;
    if (!s) {
        auto fresh = std::make_unique<TermSession>();
        s = fresh.get();
        s->owner = owner;
        // Motion starts exactly on the pill.
        const QRectF pill = m_model->pillRect();
        s->cx.reset(pill.center().x());
        s->top.reset(pill.top());
        s->w.reset(pill.width());
        s->h.reset(pill.height());
        m_terms.push_back(fresh.release());
    } else {
        retire(std::move(s->term)); // one terminal per (plugin, window)
    }
    s->command = argv.join(u' ');
    s->size = size;
    s->retracting = false;
    s->reason.clear();
    s->exitCode = -1;
    s->term = std::move(t);
    for (core::Spring *sp : {&s->cx, &s->top, &s->w, &s->h}) {
        sp->stiffness = m_s->expandStiffness;
        sp->damping = m_s->expandDamping;
        sp->epsilon = 0.2;
    }
    term::Terminal *raw = s->term.get();
    connect(raw, &term::Terminal::updated, this, [this, owner]() {
        if (TermSession *ts = session(owner)) update(terminalRect(*ts).toAlignedRect().adjusted(-2, -2, 2, 2));
    });
    connect(raw, &term::Terminal::finished, this, [this, owner](int code) {
        TermSession *ts = session(owner);
        if (!ts) return;
        ts->exitCode = code;
        retractTerminal(*ts, QStringLiteral("exited"));
    });
    qInfo().noquote() << "shade:" << (replacing ? "replaced" : "opened") << "terminal for" << owner << "on" << m_name
                      << QStringLiteral("(%1x%2, pid %3)").arg(size.width()).arg(size.height()).arg(raw->pid());
    updateCanvasSize();
    focusTerminal(s);
    updateInputRegion();
    wake();
    return true;
}

bool ShadeWindow::closeTerminal(const QString &owner, const QString &reason)
{
    TermSession *t = session(owner);
    if (!t || t->retracting) return false;
    retractTerminal(*t, reason);
    return true;
}

void ShadeWindow::retractTerminal(TermSession &t, const QString &reason)
{
    if (t.retracting) return;
    t.retracting = true;
    t.reason = reason;
    for (core::Spring *sp : {&t.cx, &t.top, &t.w, &t.h}) {
        sp->stiffness = m_s->collapseStiffness;
        sp->damping = m_s->collapseDamping;
    }
    if (m_termFocus == t.owner) {
        m_termFocus.clear();
        if (t.term) t.term->setFocused(false);
    }
    if (m_termGrab == t.owner) m_termGrab.clear();
    updateKeyboard();
    wake();
}

void ShadeWindow::retractAllTerminals(const QString &reason)
{
    for (TermSession *t : m_terms) retractTerminal(*t, reason);
}

void ShadeWindow::dropTerminal(TermSession *t, const QString &why, bool notify)
{
    const QString reason = why; // may refer into *t
    const QString owner = t->owner;
    const int code = t->exitCode;
    retire(std::move(t->term));
    m_terms.erase(std::remove(m_terms.begin(), m_terms.end(), t), m_terms.end());
    delete t;
    if (m_termFocus == owner) m_termFocus.clear();
    if (m_termGrab == owner) m_termGrab.clear();
    updateKeyboard();
    updateCanvasSize();
    updateInputRegion();
    update();
    qInfo().noquote() << "shade: closed terminal for" << owner << "on" << m_name << "(" + reason + ")";
    if (notify) emit terminalClosed(owner, reason, code);
}

void ShadeWindow::removeTerminals(const QString &owner)
{
    while (TermSession *t = session(owner)) dropTerminal(t, QStringLiteral("unloaded"), false);
}

QRectF ShadeWindow::terminalFinalRect(const TermSession &t) const
{
    const QRectF pill = m_model->pillRect();
    return QRectF(m_canvas.width() / 2.0 - t.size.width() / 2.0, pill.bottom() + m_s->terminalGap, t.size.width(), t.size.height());
}

QRectF ShadeWindow::terminalRect(const TermSession &t) const
{
    const qreal w = std::max<qreal>(0, t.w.value), h = std::max<qreal>(0, t.h.value);
    return QRectF(t.cx.value - w / 2.0, t.top.value, w, h);
}

QPointF ShadeWindow::terminalOrigin(const TermSession &t) const
{
    // Content is anchored to the viewport's top center: the viewport grows
    // around it rather than the text being squeezed.
    const QRectF r = terminalRect(t);
    const QSize content = t.term ? t.term->contentSize() : QSize();
    const qreal pad = m_s->terminalPadding;
    const qreal innerH = t.size.height() - 2 * pad;
    return QPointF(std::round(r.center().x() - content.width() / 2.0), std::round(r.top() + pad + (innerH - content.height()) / 2.0));
}

ShadeWindow::TermSession *ShadeWindow::terminalAt(const QPointF &pos)
{
    for (TermSession *t : m_terms) {
        if (!t->retracting && terminalRect(*t).contains(pos)) return t;
    }
    return nullptr;
}

bool ShadeWindow::stepTerminals(qreal dt)
{
    bool moving = false;
    const QRectF pill = m_model->pillRect();
    std::vector<TermSession *> done;
    for (TermSession *t : m_terms) {
        if (t->retracting) {
            // Back into the pill, flattening to nothing.
            t->cx.target = pill.center().x();
            t->top.target = pill.top();
            t->w.target = pill.width();
            t->h.target = 0;
        } else {
            // Follows the pill (paging, size bounces) with its own spring.
            const QRectF f = terminalFinalRect(*t);
            t->cx.target = f.center().x();
            t->top.target = f.top();
            t->w.target = f.width();
            t->h.target = f.height();
        }
        for (core::Spring *sp : {&t->cx, &t->top, &t->w, &t->h}) sp->step(dt);
        const bool settled = t->cx.settled() && t->top.settled() && t->w.settled() && t->h.settled();
        if (t->retracting && (t->h.value <= 0.5 || settled)) done.push_back(t);
        else if (!settled) moving = true;
    }
    for (TermSession *t : done) dropTerminal(t, t->reason, true);
    return moving;
}

void ShadeWindow::paintTerminals(QPainter &p)
{
    const Theme &theme = Theme::get();
    for (TermSession *t : m_terms) {
        const QRectF r = terminalRect(*t);
        if (r.width() < 0.5 || r.height() < 0.5) continue;
        const QRect bounds = r.toAlignedRect();
        const qreal radius = std::min({m_s->cornerRadius, r.height() / 2.0, r.width() / 2.0});
        QPainterPath shape;
        shape.addRoundedRect(r, radius, radius);
        // Composite into a layer so the rounded clip is antialiased.
        const qreal dpr = devicePixelRatioF();
        QImage layer(bounds.size() * dpr, QImage::Format_ARGB32_Premultiplied);
        layer.setDevicePixelRatio(dpr);
        layer.fill(Qt::transparent);
        {
            QPainter lp(&layer);
            lp.setRenderHint(QPainter::Antialiasing);
            lp.translate(-bounds.topLeft());
            lp.fillPath(shape, theme.bg());
            if (t->term) t->term->paint(&lp, terminalOrigin(*t));
            lp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            lp.fillPath(shape, Qt::black);
        }
        p.drawImage(bounds.topLeft(), layer);
    }
}

void ShadeWindow::focusTerminal(TermSession *t)
{
    const QString owner = t ? t->owner : QString();
    for (TermSession *s : m_terms) {
        if (s->term) s->term->setFocused(s == t && isActiveWindow());
    }
    m_termFocus = owner;
    updateKeyboard();
    if (t) {
        activateWindow();
        if (QWindow *w = windowHandle()) w->requestActivate();
    }
}

void ShadeWindow::updateKeyboard()
{
    const bool want = std::any_of(m_terms.cbegin(), m_terms.cend(), [](const TermSession *t) { return !t->retracting; });
    if (want == m_keyboardWanted) return;
    m_keyboardWanted = want;
    setFocusPolicy(want ? Qt::StrongFocus : Qt::NoFocus);
#ifdef SHADE_HAVE_LAYER_SHELL
    if (m_layer) {
        using LW = LayerShellQt::Window;
        m_layer->setKeyboardInteractivity(!want ? LW::KeyboardInteractivityNone
                                          : m_s->terminalKeyboard == QLatin1String("exclusive") ? LW::KeyboardInteractivityExclusive
                                                                                                : LW::KeyboardInteractivityOnDemand);
    }
#endif
}

QJsonArray ShadeWindow::terminalsJson(bool withText) const
{
    QJsonArray out;
    for (TermSession *t : m_terms) {
        const QRectF r = terminalRect(*t);
        QJsonObject o{
            {QStringLiteral("owner"), t->owner},
            {QStringLiteral("command"), t->command},
            {QStringLiteral("state"), t->retracting ? QStringLiteral("retracting") : QStringLiteral("presented")},
            {QStringLiteral("focused"), m_termFocus == t->owner},
            {QStringLiteral("rect"), QJsonObject{{QStringLiteral("x"), r.x()}, {QStringLiteral("y"), r.y()}, {QStringLiteral("w"), r.width()}, {QStringLiteral("h"), r.height()}}},
            {QStringLiteral("size"), QJsonObject{{QStringLiteral("w"), t->size.width()}, {QStringLiteral("h"), t->size.height()}}},
        };
        if (t->term) {
            o.insert(QStringLiteral("pid"), t->term->pid());
            o.insert(QStringLiteral("running"), t->term->running());
            o.insert(QStringLiteral("grid"), QJsonObject{{QStringLiteral("cols"), t->term->gridSize().width()}, {QStringLiteral("rows"), t->term->gridSize().height()}});
            if (withText) o.insert(QStringLiteral("text"), t->term->screenText());
        }
        out << o;
    }
    return out;
}

bool ShadeWindow::typeIntoTerminal(const QString &owner, const QString &text)
{
    TermSession *t = owner.isEmpty() ? (m_terms.empty() ? nullptr : m_terms.front()) : session(owner);
    if (!t || !t->term || t->retracting) return false;
    t->term->sendText(text);
    return true;
}

// --- input -------------------------------------------------------------------------

bool ShadeWindow::terminalPointer(QEvent::Type type, const QPointF &pos, Qt::MouseButton button, Qt::KeyboardModifiers mods)
{
    TermSession *t = nullptr;
    if (!m_termGrab.isEmpty() && type != QEvent::MouseButtonPress) t = session(m_termGrab);
    if (!t) t = terminalAt(pos);
    if (!t || !t->term) return false;
    const QPointF local = pos - terminalOrigin(*t);
    switch (type) {
    case QEvent::MouseButtonPress:
        focusTerminal(t);
        m_termGrab = t->owner;
        t->term->mouseButton(local, button, true, mods);
        break;
    case QEvent::MouseButtonRelease:
        t->term->mouseButton(local, button, false, mods);
        m_termGrab.clear();
        break;
    default: t->term->mouseMove(local, mods); break;
    }
    return true;
}

bool ShadeWindow::terminalWheel(const QPointF &pos, const QPointF &delta, Qt::KeyboardModifiers mods)
{
    TermSession *t = terminalAt(pos);
    if (!t || !t->term) return false;
    m_termWheel += delta.y();
    const int steps = int(m_termWheel / m_s->pixelsPerWheelNotch);
    if (steps != 0) {
        m_termWheel -= steps * m_s->pixelsPerWheelNotch;
        t->term->wheel(pos - terminalOrigin(*t), steps, mods);
    }
    return true;
}

bool ShadeWindow::terminalKey(QKeyEvent *e)
{
    TermSession *t = session(m_termFocus);
    if (!t || !t->term || t->retracting) return false;
    if (e->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && e->key() == Qt::Key_V) {
        t->term->paste(QGuiApplication::clipboard()->text());
        return true;
    }
    if (e->modifiers() == Qt::ShiftModifier && e->key() == Qt::Key_Insert) {
        t->term->paste(QGuiApplication::clipboard()->text(QClipboard::Selection));
        return true;
    }
    return t->term->sendKey(e);
}

void ShadeWindow::terminalsFocusChanged(bool active)
{
    for (TermSession *s : m_terms) {
        if (s->term) s->term->setFocused(active && s->owner == m_termFocus);
    }
}

QRectF ShadeWindow::terminalsBounds() const
{
    QRectF r;
    const QRectF pill = m_model->pillRect();
    for (TermSession *t : m_terms) {
        QRectF tr = terminalRect(*t);
        if (!t->retracting) tr = tr.united(terminalFinalRect(*t));
        // Bridge the gap so pill -> terminal never leaves the input region.
        tr = tr.united(QRectF(tr.left(), pill.bottom() - 2, tr.width(), std::max<qreal>(0, tr.top() - pill.bottom() + 2)));
        r = r.united(tr);
    }
    return r;
}

QSize ShadeWindow::terminalsCanvasNeed() const
{
    QSize need;
    for (TermSession *t : m_terms) {
        need = need.expandedTo(QSize(t->size.width() + 48,
                                     int(std::ceil(m_s->topOffset + maxPaneHeight() * 1.25 + m_s->terminalGap + t->size.height() * 1.08 + 32))));
    }
    return need;
}

void ShadeWindow::destroyTerminals()
{
    for (TermSession *t : m_terms) delete t; // synchronous: kill and reap
    m_terms.clear();
}

} // namespace shade::app
