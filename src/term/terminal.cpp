#include "terminal.h"

#include <shade/theme.h>

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPainter>
#include <QSocketNotifier>
#include <QTimer>

#include <vterm.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmath>

extern char **environ;

namespace shade::term
{

// --- libvterm callbacks --------------------------------------------------------

struct Callbacks
{
    static int damage(VTermRect r, void *user)
    {
        return static_cast<Terminal *>(user)->onDamage(r.start_row, r.end_row, r.start_col, r.end_col);
    }
    static int moverect(VTermRect, VTermRect, void *) { return 0; } // fall back to damage
    static int movecursor(VTermPos pos, VTermPos, int visible, void *user)
    {
        return static_cast<Terminal *>(user)->onMoveCursor(pos.row, pos.col, visible);
    }
    static int settermprop(VTermProp prop, VTermValue *val, void *user)
    {
        auto *t = static_cast<Terminal *>(user);
        switch (prop) {
        case VTERM_PROP_TITLE: {
            const QString s = QString::fromUtf8(val->string.str, int(val->string.len));
            if (val->string.initial) t->m_title.clear();
            t->m_title += s;
            if (val->string.final) emit t->titleChanged(t->m_title);
            return 1;
        }
        default: return t->onProp(prop, val);
        }
    }
    static int bell(void *) { return 1; }
    static int resize(int, int, void *) { return 1; }
    static int sbPushline(int cols, const VTermScreenCell *cells, void *user)
    {
        return static_cast<Terminal *>(user)->onPushLine(cols, cells);
    }
    static int sbPopline(int cols, VTermScreenCell *cells, void *user)
    {
        return static_cast<Terminal *>(user)->onPopLine(cols, cells);
    }
    static int sbClear(void *user)
    {
        auto *t = static_cast<Terminal *>(user);
        t->m_scrollback.clear();
        t->m_offset = 0;
        return 1;
    }
    static void output(const char *s, size_t len, void *user) { static_cast<Terminal *>(user)->onOutput(s, len); }
};

namespace
{
const VTermScreenCallbacks kScreenCallbacks = {
    Callbacks::damage, Callbacks::moverect, Callbacks::movecursor, Callbacks::settermprop, Callbacks::bell,
    Callbacks::resize, Callbacks::sbPushline, Callbacks::sbPopline, Callbacks::sbClear,
};

VTermColor vcolor(const QColor &c)
{
    VTermColor v;
    vterm_color_rgb(&v, uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()));
    return v;
}

QFont styled(QFont f, bool bold, bool italic)
{
    f.setBold(bold);
    f.setItalic(italic);
    return f;
}
} // namespace

Palette themePalette()
{
    const Theme &t = Theme::get();
    Palette p;
    p.foreground = t.tx();
    p.background = t.bg();
    p.cursor = t.tx();
    const QColor normal[8] = {t.ui(), t.color(QStringLiteral("red")), t.color(QStringLiteral("green")), t.color(QStringLiteral("yellow")),
                              t.color(QStringLiteral("blue")), t.color(QStringLiteral("magenta")), t.color(QStringLiteral("cyan")), t.tx2()};
    for (int i = 0; i < 8; ++i) {
        p.ansi[i] = normal[i];
        p.ansi[i + 8] = i == 0 ? t.ui3() : i == 7 ? t.tx() : normal[i].lighter(125);
    }
    return p;
}

// --- construction -------------------------------------------------------------

Terminal::Terminal(const QFont &font, const Palette &palette, int scrollback, QObject *parent)
    : QObject(parent)
    , m_font(font)
    , m_palette(palette)
    , m_scrollbackMax(std::clamp(scrollback, 0, 100000))
{
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    m_font.setKerning(false);
    m_bold = styled(m_font, true, false);
    m_italic = styled(m_font, false, true);
    m_boldItalic = styled(m_font, true, true);
    const QFontMetricsF fm(m_font);
    m_cell = QSize(int(std::ceil(fm.horizontalAdvance(QLatin1Char('M')))), int(std::ceil(fm.height())));
    if (m_cell.width() <= 0) m_cell.setWidth(8);
    if (m_cell.height() <= 0) m_cell.setHeight(16);

    m_vt = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt, &Callbacks::output, this);
    m_screen = vterm_obtain_screen(m_vt);
    vterm_screen_set_callbacks(m_screen, &kScreenCallbacks, this);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_enable_reflow(m_screen, true);
    VTermState *state = vterm_obtain_state(m_vt);
    const VTermColor fg = vcolor(m_palette.foreground), bg = vcolor(m_palette.background);
    vterm_state_set_default_colors(state, &fg, &bg);
    for (int i = 0; i < 16; ++i) {
        if (!m_palette.ansi[i].isValid()) continue;
        const VTermColor c = vcolor(m_palette.ansi[i]);
        vterm_state_set_palette_color(state, i, &c);
    }
    vterm_state_set_bold_highbright(state, 1);
    vterm_screen_reset(m_screen, 1);

    m_reaper = new QTimer(this);
    m_reaper->setInterval(200);
    connect(m_reaper, &QTimer::timeout, this, &Terminal::checkChild);
    regrid(m_rows, m_cols);
}

Terminal::~Terminal()
{
    if (m_pid > 0 && !m_exited) {
        ::kill(-pid_t(m_pid), SIGHUP);
        ::kill(pid_t(m_pid), SIGHUP);
        // Give it a moment to exit cleanly, then make sure.
        for (int i = 0; i < 20; ++i) {
            if (::waitpid(pid_t(m_pid), nullptr, WNOHANG) == pid_t(m_pid)) {
                m_exited = true;
                break;
            }
            ::usleep(10'000);
        }
        if (!m_exited) {
            ::kill(-pid_t(m_pid), SIGKILL);
            ::kill(pid_t(m_pid), SIGKILL);
            ::waitpid(pid_t(m_pid), nullptr, 0);
        }
    }
    if (m_master >= 0) ::close(m_master);
    if (m_vt) vterm_free(m_vt);
}

// --- process --------------------------------------------------------------------

bool Terminal::start(const QStringList &argv, const QSize &pixels, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    if (m_pid > 0) return fail(QStringLiteral("already started"));
    if (argv.isEmpty() || argv.first().isEmpty()) return fail(QStringLiteral("empty command"));

    const int cols = std::max(2, pixels.width() / m_cell.width());
    const int rows = std::max(1, pixels.height() / m_cell.height());
    regrid(rows, cols);

    // Everything the child needs is prepared before fork(): after it only
    // async-signal-safe calls are allowed.
    std::vector<QByteArray> args;
    for (const QString &a : argv) args.push_back(a.toLocal8Bit());
    std::vector<char *> cargv;
    for (QByteArray &a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);

    std::vector<QByteArray> env;
    for (char **e = environ; e && *e; ++e) {
        const QByteArray kv(*e);
        if (kv.startsWith("TERM=") || kv.startsWith("COLORTERM=") || kv.startsWith("COLUMNS=") || kv.startsWith("LINES=")) continue;
        env.push_back(kv);
    }
    env.push_back("TERM=xterm-256color");
    env.push_back("COLORTERM=truecolor");
    std::vector<char *> cenv;
    for (QByteArray &e : env) cenv.push_back(e.data());
    cenv.push_back(nullptr);

    // Exec-failure channel: closed on a successful exec (CLOEXEC); the child
    // writes errno into it when exec fails.
    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) != 0) return fail(QString::fromLocal8Bit(std::strerror(errno)));

    struct winsize ws {};
    ws.ws_row = uint16_t(m_rows);
    ws.ws_col = uint16_t(m_cols);
    ws.ws_xpixel = uint16_t(m_cols * m_cell.width());
    ws.ws_ypixel = uint16_t(m_rows * m_cell.height());
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return fail(QStringLiteral("forkpty: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
    if (pid == 0) {
        // Child: a session leader on the pty slave (forkpty did setsid).
        ::close(pipefd[0]);
        sigset_t all;
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, nullptr);
        for (int sig : {SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGCHLD}) ::signal(sig, SIG_DFL);
        // Do not leak the shade's descriptors into the command.
        const int keep = pipefd[1];
        for (int fd = 3; fd < 1024; ++fd) {
            if (fd != keep) ::close(fd);
        }
        ::execvpe(cargv[0], cargv.data(), cenv.data());
        const int err = errno;
        [[maybe_unused]] ssize_t n = ::write(keep, &err, sizeof err);
        ::_exit(127);
    }
    ::close(pipefd[1]);
    int childErr = 0;
    ssize_t n;
    do {
        n = ::read(pipefd[0], &childErr, sizeof childErr);
    } while (n < 0 && errno == EINTR);
    ::close(pipefd[0]);
    if (n == sizeof childErr) {
        ::close(master);
        ::waitpid(pid, nullptr, 0);
        return fail(QStringLiteral("cannot run %1: %2").arg(argv.first(), QString::fromLocal8Bit(std::strerror(childErr))));
    }

    m_master = master;
    m_pid = pid;
    ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL) | O_NONBLOCK);
    ::fcntl(m_master, F_SETFD, FD_CLOEXEC);
    m_readNotifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &Terminal::readPty);
    m_writeNotifier = new QSocketNotifier(m_master, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &Terminal::flushWrites);
    m_reaper->start();
    return true;
}

void Terminal::readPty()
{
    char buf[16384];
    bool any = false;
    for (int round = 0; round < 16; ++round) {
        const ssize_t n = ::read(m_master, buf, sizeof buf);
        if (n > 0) {
            vterm_input_write(m_vt, buf, size_t(n));
            any = true;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        // EOF / EIO: every slave descriptor is gone.
        m_readNotifier->setEnabled(false);
        checkChild();
        break;
    }
    if (any) {
        vterm_screen_flush_damage(m_screen);
        if (m_dirtyTop >= 0) {
            redraw(m_dirtyTop, m_dirtyBottom);
            m_dirtyTop = m_dirtyBottom = -1;
        }
        emit updated();
    }
}

void Terminal::checkChild()
{
    if (m_pid <= 0 || m_exited) return;
    int status = 0;
    const pid_t r = ::waitpid(pid_t(m_pid), &status, WNOHANG);
    if (r != pid_t(m_pid)) return;
    m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    finish();
}

void Terminal::finish()
{
    if (m_exited) return;
    m_exited = true;
    m_reaper->stop();
    // Drain what the command printed last.
    if (m_readNotifier && m_readNotifier->isEnabled()) readPty();
    if (m_readNotifier) m_readNotifier->setEnabled(false);
    if (m_writeNotifier) m_writeNotifier->setEnabled(false);
    emit finished(m_exitCode);
}

void Terminal::terminate()
{
    if (m_pid <= 0 || m_exited) return;
    ::kill(-pid_t(m_pid), SIGHUP);
    ::kill(pid_t(m_pid), SIGHUP);
    QTimer::singleShot(400, this, [this]() {
        if (m_exited) return;
        ::kill(-pid_t(m_pid), SIGKILL);
        ::kill(pid_t(m_pid), SIGKILL);
    });
}

void Terminal::onOutput(const char *bytes, size_t len)
{
    if (m_master < 0 || m_exited) return;
    m_pendingWrite.append(bytes, qsizetype(len));
    flushWrites();
}

void Terminal::flushWrites()
{
    while (!m_pendingWrite.isEmpty()) {
        const ssize_t n = ::write(m_master, m_pendingWrite.constData(), size_t(m_pendingWrite.size()));
        if (n > 0) {
            m_pendingWrite.remove(0, qsizetype(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (m_writeNotifier) m_writeNotifier->setEnabled(true);
            return;
        }
        m_pendingWrite.clear(); // the child is gone
        break;
    }
    if (m_writeNotifier) m_writeNotifier->setEnabled(false);
}

// --- geometry ---------------------------------------------------------------------

void Terminal::regrid(int rows, int cols)
{
    rows = std::clamp(rows, 1, 1000);
    cols = std::clamp(cols, 2, 1000);
    const bool changed = rows != m_rows || cols != m_cols || m_image.isNull();
    m_rows = rows;
    m_cols = cols;
    if (!changed) return;
    vterm_set_size(m_vt, rows, cols);
    if (m_master >= 0) {
        struct winsize ws {};
        ws.ws_row = uint16_t(rows);
        ws.ws_col = uint16_t(cols);
        ws.ws_xpixel = uint16_t(cols * m_cell.width());
        ws.ws_ypixel = uint16_t(rows * m_cell.height());
        ::ioctl(m_master, TIOCSWINSZ, &ws); // the kernel sends SIGWINCH
    }
    m_image = QImage(contentSize(), QImage::Format_ARGB32_Premultiplied);
    m_offset = std::min<int>(m_offset, int(m_scrollback.size()));
    redraw(0, m_rows);
    emit updated();
}

void Terminal::setPixelSize(const QSize &pixels)
{
    regrid(std::max(1, pixels.height() / m_cell.height()), std::max(2, pixels.width() / m_cell.width()));
}

// --- screen model -------------------------------------------------------------------

int Terminal::onDamage(int r0, int r1, int, int)
{
    m_dirtyTop = m_dirtyTop < 0 ? r0 : std::min(m_dirtyTop, r0);
    m_dirtyBottom = std::max(m_dirtyBottom, r1);
    return 1;
}

int Terminal::onMoveCursor(int row, int col, bool visible)
{
    m_cursor = QPoint(col, row);
    m_cursorVisible = visible;
    return 1;
}

int Terminal::onProp(int prop, const void *value)
{
    const auto *v = static_cast<const VTermValue *>(value);
    switch (VTermProp(prop)) {
    case VTERM_PROP_CURSORVISIBLE: m_cursorVisible = v->boolean; break;
    case VTERM_PROP_CURSORSHAPE: m_cursorShape = v->number; break;
    case VTERM_PROP_ALTSCREEN:
        m_altScreen = v->boolean;
        m_offset = 0;
        break;
    case VTERM_PROP_MOUSE: m_mouseMode = v->number; break;
    default: break;
    }
    return 1;
}

int Terminal::onPushLine(int cols, const void *cells)
{
    if (m_scrollbackMax == 0) return 0;
    const auto *vc = static_cast<const VTermScreenCell *>(cells);
    Line line;
    line.reserve(size_t(cols));
    for (int i = 0; i < cols; ++i) line.push_back(cellFromVTerm(&vc[i]));
    m_scrollback.push_back(std::move(line));
    if (int(m_scrollback.size()) > m_scrollbackMax) m_scrollback.pop_front();
    // Someone reading the scrollback keeps their place while output scrolls.
    if (m_offset > 0) m_offset = std::min<int>(m_offset + 1, int(m_scrollback.size()));
    return 1;
}

int Terminal::onPopLine(int cols, void *cells)
{
    // Reflow on grow: give back lines from the scrollback.
    if (m_scrollback.empty()) return 0;
    auto *vc = static_cast<VTermScreenCell *>(cells);
    const Line line = std::move(m_scrollback.back());
    m_scrollback.pop_back();
    for (int i = 0; i < cols; ++i) {
        VTermScreenCell &c = vc[i];
        std::memset(&c, 0, sizeof c);
        c.width = 1;
        vterm_color_rgb(&c.fg, uint8_t(m_palette.foreground.red()), uint8_t(m_palette.foreground.green()), uint8_t(m_palette.foreground.blue()));
        c.fg.type |= VTERM_COLOR_DEFAULT_FG;
        vterm_color_rgb(&c.bg, 0, 0, 0);
        c.bg.type |= VTERM_COLOR_DEFAULT_BG;
        if (i < int(line.size())) {
            const Cell &src = line[size_t(i)];
            for (int k = 0; k < 6; ++k) c.chars[k] = src.ch[k];
            c.width = char(src.width);
            c.attrs.bold = src.bold;
            c.attrs.italic = src.italic;
            c.attrs.underline = src.underline ? 1 : 0;
            c.attrs.strike = src.strike;
            c.attrs.reverse = src.reverse;
            if (src.fg.isValid() && src.fg != m_palette.foreground) vterm_color_rgb(&c.fg, uint8_t(src.fg.red()), uint8_t(src.fg.green()), uint8_t(src.fg.blue()));
            if (src.bg.isValid()) vterm_color_rgb(&c.bg, uint8_t(src.bg.red()), uint8_t(src.bg.green()), uint8_t(src.bg.blue()));
        }
    }
    return 1;
}

QColor Terminal::resolve(const void *vc, bool fg) const
{
    VTermColor c = *static_cast<const VTermColor *>(vc);
    if (fg && VTERM_COLOR_IS_DEFAULT_FG(&c)) return m_palette.foreground;
    if (!fg && VTERM_COLOR_IS_DEFAULT_BG(&c)) return QColor(); // transparent
    if (VTERM_COLOR_IS_INDEXED(&c)) vterm_screen_convert_color_to_rgb(m_screen, &c);
    return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
}

Terminal::Cell Terminal::cellFromVTerm(const void *p) const
{
    const auto *vc = static_cast<const VTermScreenCell *>(p);
    Cell c;
    for (int k = 0; k < 6; ++k) c.ch[k] = vc->chars[k];
    c.width = std::max(1, int(vc->width));
    c.fg = resolve(&vc->fg, true);
    c.bg = resolve(&vc->bg, false);
    c.bold = vc->attrs.bold;
    c.italic = vc->attrs.italic;
    c.underline = vc->attrs.underline != 0;
    c.strike = vc->attrs.strike;
    c.reverse = vc->attrs.reverse;
    c.conceal = vc->attrs.conceal;
    return c;
}

Terminal::Cell Terminal::cellAt(int row, int col) const
{
    // View row -> scrollback line or live screen row.
    const int sb = int(m_scrollback.size());
    const int line = row - m_offset; // < 0: inside the scrollback
    if (line < 0) {
        const int idx = sb + line;
        if (idx < 0 || idx >= sb) return {};
        const Line &l = m_scrollback[size_t(idx)];
        return col < int(l.size()) ? l[size_t(col)] : Cell{};
    }
    VTermScreenCell vc;
    if (!vterm_screen_get_cell(m_screen, VTermPos{line, col}, &vc)) return {};
    return cellFromVTerm(&vc);
}

void Terminal::redraw(int r0, int r1)
{
    if (m_image.isNull()) return;
    r0 = std::clamp(r0, 0, m_rows);
    r1 = std::clamp(r1, 0, m_rows);
    if (r0 >= r1) return;
    QPainter p(&m_image);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(QRect(0, r0 * m_cell.height(), m_image.width(), (r1 - r0) * m_cell.height()), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setRenderHint(QPainter::TextAntialiasing);
    for (int row = r0; row < r1; ++row) {
        for (int col = 0; col < m_cols;) {
            const Cell c = cellAt(row, col);
            paintCell(p, c, row, col);
            col += std::max(1, c.width);
        }
    }
}

void Terminal::paintCell(QPainter &p, const Cell &c, int row, int col) const
{
    QColor fg = c.fg.isValid() ? c.fg : m_palette.foreground;
    QColor bg = c.bg;
    if (c.reverse) {
        const QColor f = fg;
        fg = bg.isValid() ? bg : m_palette.background;
        bg = f;
    }
    const QRect r(col * m_cell.width(), row * m_cell.height(), m_cell.width() * std::max(1, c.width), m_cell.height());
    if (bg.isValid()) p.fillRect(r, bg);
    if (c.ch[0] == 0 || c.ch[0] == ' ' || c.conceal) {
        if (!c.underline && !c.strike) return;
    }
    QString text;
    for (int k = 0; k < 6 && c.ch[k]; ++k) text += QString::fromUcs4(reinterpret_cast<const char32_t *>(&c.ch[k]), 1);
    p.setPen(fg);
    if (!text.isEmpty() && !c.conceal) {
        p.setFont(c.bold ? (c.italic ? m_boldItalic : m_bold) : (c.italic ? m_italic : m_font));
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextDontClip, text);
    }
    if (c.underline) p.fillRect(QRect(r.left(), r.bottom() - 1, r.width(), 1), fg);
    if (c.strike) p.fillRect(QRect(r.left(), r.top() + r.height() / 2, r.width(), 1), fg);
}

void Terminal::paint(QPainter *p, const QPointF &origin) const
{
    p->drawImage(origin, m_image);
    if (!m_cursorVisible || m_offset != 0 || m_exited) return;
    const QRectF cell(origin.x() + m_cursor.x() * m_cell.width(), origin.y() + m_cursor.y() * m_cell.height(), m_cell.width(), m_cell.height());
    p->save();
    if (!m_focused) {
        p->setPen(QPen(m_palette.cursor, 1));
        p->setBrush(Qt::NoBrush);
        p->drawRect(cell.adjusted(0.5, 0.5, -0.5, -0.5));
    } else if (m_cursorShape == VTERM_PROP_CURSORSHAPE_UNDERLINE) {
        p->fillRect(QRectF(cell.left(), cell.bottom() - 2, cell.width(), 2), m_palette.cursor);
    } else if (m_cursorShape == VTERM_PROP_CURSORSHAPE_BAR_LEFT) {
        p->fillRect(QRectF(cell.left(), cell.top(), 2, cell.height()), m_palette.cursor);
    } else {
        // Block: inverted glyph.
        p->fillRect(cell, m_palette.cursor);
        const Cell c = cellAt(m_cursor.y(), m_cursor.x());
        if (c.ch[0] && c.ch[0] != ' ') {
            QString text;
            for (int k = 0; k < 6 && c.ch[k]; ++k) text += QString::fromUcs4(reinterpret_cast<const char32_t *>(&c.ch[k]), 1);
            p->setFont(c.bold ? m_bold : m_font);
            p->setPen(m_palette.background);
            p->drawText(cell, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextDontClip, text);
        }
    }
    p->restore();
}

void Terminal::scrollView(int lines)
{
    if (m_altScreen) return;
    const int next = std::clamp(m_offset + lines, 0, int(m_scrollback.size()));
    if (next == m_offset) return;
    m_offset = next;
    redraw(0, m_rows);
    emit updated();
}

void Terminal::snapToBottom()
{
    if (m_offset == 0) return;
    m_offset = 0;
    redraw(0, m_rows);
    emit updated();
}

void Terminal::setFocused(bool focused)
{
    if (focused == m_focused) return;
    m_focused = focused;
    emit updated();
}

// --- inspection -------------------------------------------------------------------

QString Terminal::lineText(int row) const
{
    QString out;
    for (int col = 0; col < m_cols;) {
        const Cell c = cellAt(row, col);
        if (c.ch[0] == 0) out += u' ';
        else
            for (int k = 0; k < 6 && c.ch[k]; ++k) out += QString::fromUcs4(reinterpret_cast<const char32_t *>(&c.ch[k]), 1);
        col += std::max(1, c.width);
    }
    while (out.endsWith(u' ')) out.chop(1);
    return out;
}

QString Terminal::screenText() const
{
    QStringList lines;
    for (int row = 0; row < m_rows; ++row) lines << lineText(row);
    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines.join(u'\n');
}

QColor Terminal::foregroundAt(int row, int col) const
{
    const Cell c = cellAt(row, col);
    return c.fg.isValid() ? c.fg : m_palette.foreground;
}

QColor Terminal::backgroundAt(int row, int col) const { return cellAt(row, col).bg; }

// --- input --------------------------------------------------------------------------

int Terminal::toVTermMods(Qt::KeyboardModifiers mods) const
{
    int m = VTERM_MOD_NONE;
    if (mods & Qt::ShiftModifier) m |= VTERM_MOD_SHIFT;
    if (mods & Qt::AltModifier) m |= VTERM_MOD_ALT;
    if (mods & Qt::ControlModifier) m |= VTERM_MOD_CTRL;
    return m;
}

bool Terminal::sendKey(const QKeyEvent *e)
{
    if (m_exited) return false;
    const VTermModifier mod = VTermModifier(toVTermMods(e->modifiers()));
    const bool keypad = e->modifiers() & Qt::KeypadModifier;
    VTermKey key = VTERM_KEY_NONE;
    switch (e->key()) {
    case Qt::Key_Return: key = VTERM_KEY_ENTER; break;
    case Qt::Key_Enter: key = keypad ? VTERM_KEY_KP_ENTER : VTERM_KEY_ENTER; break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: key = VTERM_KEY_TAB; break;
    case Qt::Key_Backspace: key = VTERM_KEY_BACKSPACE; break;
    case Qt::Key_Escape: key = VTERM_KEY_ESCAPE; break;
    case Qt::Key_Up: key = VTERM_KEY_UP; break;
    case Qt::Key_Down: key = VTERM_KEY_DOWN; break;
    case Qt::Key_Left: key = VTERM_KEY_LEFT; break;
    case Qt::Key_Right: key = VTERM_KEY_RIGHT; break;
    case Qt::Key_Insert: key = VTERM_KEY_INS; break;
    case Qt::Key_Delete: key = VTERM_KEY_DEL; break;
    case Qt::Key_Home: key = VTERM_KEY_HOME; break;
    case Qt::Key_End: key = VTERM_KEY_END; break;
    case Qt::Key_PageUp: key = VTERM_KEY_PAGEUP; break;
    case Qt::Key_PageDown: key = VTERM_KEY_PAGEDOWN; break;
    default:
        if (e->key() >= Qt::Key_F1 && e->key() <= Qt::Key_F35) key = VTermKey(VTERM_KEY_FUNCTION(e->key() - Qt::Key_F1 + 1));
        break;
    }
    // Shift+PageUp/Down scroll the view (like most terminals).
    if ((key == VTERM_KEY_PAGEUP || key == VTERM_KEY_PAGEDOWN) && (e->modifiers() & Qt::ShiftModifier) && !m_altScreen) {
        scrollView(key == VTERM_KEY_PAGEUP ? m_rows / 2 : -m_rows / 2);
        return true;
    }
    if (key != VTERM_KEY_NONE) {
        snapToBottom();
        vterm_keyboard_key(m_vt, key, e->key() == Qt::Key_Backtab ? VTermModifier(mod | VTERM_MOD_SHIFT) : mod);
        return true;
    }
    const bool ctrlOrAlt = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier);
    if (ctrlOrAlt) {
        // Base character for Ctrl/Alt combinations (the event text already
        // holds the control code).
        uint32_t c = 0;
        if (e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z) c = uint32_t('a' + (e->key() - Qt::Key_A));
        else if (e->key() >= 0x20 && e->key() < 0x7f) c = uint32_t(e->key());
        else if (!e->text().isEmpty() && e->text().at(0).unicode() >= 0x20) c = e->text().at(0).unicode();
        if (c == 0) return false;
        snapToBottom();
        vterm_keyboard_unichar(m_vt, c, mod);
        return true;
    }
    if (e->text().isEmpty()) return false;
    snapToBottom();
    for (const char32_t cp : e->text().toUcs4()) {
        if (cp < 0x20 || cp == 0x7f) continue;
        vterm_keyboard_unichar(m_vt, uint32_t(cp), VTERM_MOD_NONE);
    }
    return true;
}

void Terminal::sendText(const QString &text)
{
    if (m_exited) return;
    snapToBottom();
    for (const char32_t cp : text.toUcs4()) {
        if (cp == '\n' || cp == '\r') vterm_keyboard_key(m_vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
        else if (cp == '\t') vterm_keyboard_key(m_vt, VTERM_KEY_TAB, VTERM_MOD_NONE);
        else if (cp >= 0x20) vterm_keyboard_unichar(m_vt, uint32_t(cp), VTERM_MOD_NONE);
    }
}

void Terminal::paste(const QString &text)
{
    if (m_exited || text.isEmpty()) return;
    snapToBottom();
    vterm_keyboard_start_paste(m_vt);
    QString t = text;
    t.replace(QLatin1String("\r\n"), QLatin1String("\r")).replace(u'\n', u'\r');
    const QByteArray bytes = t.toUtf8();
    onOutput(bytes.constData(), size_t(bytes.size()));
    vterm_keyboard_end_paste(m_vt);
}

QPoint Terminal::cellPos(const QPointF &pos) const
{
    return QPoint(std::clamp(int(pos.x() / m_cell.width()), 0, m_cols - 1), std::clamp(int(pos.y() / m_cell.height()), 0, m_rows - 1));
}

void Terminal::mouseMove(const QPointF &pos, Qt::KeyboardModifiers mods)
{
    if (m_exited || m_mouseMode == 0) return;
    const QPoint c = cellPos(pos);
    vterm_mouse_move(m_vt, c.y(), c.x(), VTermModifier(toVTermMods(mods)));
}

void Terminal::mouseButton(const QPointF &pos, Qt::MouseButton button, bool pressed, Qt::KeyboardModifiers mods)
{
    if (m_exited || m_mouseMode == 0) return;
    const int b = button == Qt::LeftButton ? 1 : button == Qt::MiddleButton ? 2 : button == Qt::RightButton ? 3 : 0;
    if (!b) return;
    const QPoint c = cellPos(pos);
    const VTermModifier mod = VTermModifier(toVTermMods(mods));
    vterm_mouse_move(m_vt, c.y(), c.x(), mod);
    vterm_mouse_button(m_vt, b, pressed, mod);
}

void Terminal::wheel(const QPointF &pos, int steps, Qt::KeyboardModifiers mods)
{
    if (steps == 0) return;
    if (m_exited) {
        scrollView(steps * 3); // output stays readable after the command ends
    } else if (m_mouseMode != 0) {
        const QPoint c = cellPos(pos);
        const VTermModifier mod = VTermModifier(toVTermMods(mods));
        vterm_mouse_move(m_vt, c.y(), c.x(), mod);
        for (int i = 0; i < std::abs(steps); ++i) {
            vterm_mouse_button(m_vt, steps > 0 ? 4 : 5, true, mod);
            vterm_mouse_button(m_vt, steps > 0 ? 4 : 5, false, mod);
        }
    } else if (m_altScreen) {
        // Full-screen app without mouse support: wheel = arrow keys.
        for (int i = 0; i < std::abs(steps) * 3; ++i) vterm_keyboard_key(m_vt, steps > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
    } else {
        scrollView(steps * 3);
    }
}

} // namespace shade::term
