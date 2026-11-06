#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QObject>
#include <QStringList>

#include <deque>
#include <vector>

class QKeyEvent;
class QPainter;
class QSocketNotifier;
class QTimer;

struct VTerm;
struct VTermScreen;

namespace shade::term
{

struct Palette
{
    QColor foreground{0xCE, 0xCD, 0xC3};
    QColor background{0x10, 0x0F, 0x0F};
    QColor cursor{0xCE, 0xCD, 0xC3};
    QColor ansi[16];
};

// An interactive terminal: a child process on a pseudo-terminal, a
// libvterm screen, and a cell renderer into an image the host paints
// wherever it likes (the shade composites it into an animated viewport).
//
// The command is executed directly (argv, no shell). Default-background
// cells stay transparent so the host's rounded surface shows through.
class Terminal : public QObject
{
    Q_OBJECT

public:
    Terminal(const QFont &font, const Palette &palette, int scrollback = 2000, QObject *parent = nullptr);
    ~Terminal() override; // terminates the child (SIGHUP, then SIGKILL)

    // Spawns `argv` on a new pty sized to fit `pixels`. Fails (returning
    // false, no child left behind) when the program cannot be executed.
    bool start(const QStringList &argv, const QSize &pixels, QString *error = nullptr);

    // Re-grid to a new pixel size (the child gets SIGWINCH).
    void setPixelSize(const QSize &pixels);

    QSize gridSize() const { return QSize(m_cols, m_rows); }
    QSize cellSize() const { return m_cell; }
    QSize contentSize() const { return QSize(m_cols * m_cell.width(), m_rows * m_cell.height()); }
    // Draws the screen (and the cursor) with its top-left at `origin`.
    void paint(QPainter *p, const QPointF &origin) const;

    // Input. Positions are relative to the content's top-left.
    bool sendKey(const QKeyEvent *event);
    void sendText(const QString &text);
    void paste(const QString &text);
    void mouseMove(const QPointF &pos, Qt::KeyboardModifiers mods);
    void mouseButton(const QPointF &pos, Qt::MouseButton button, bool pressed, Qt::KeyboardModifiers mods);
    void wheel(const QPointF &pos, int steps, Qt::KeyboardModifiers mods); // steps > 0: up
    void setFocused(bool focused);
    bool focused() const { return m_focused; }

    // Scrollback view: lines scrolled back from the live screen.
    int scrollOffset() const { return m_offset; }
    int scrollbackLines() const { return int(m_scrollback.size()); }
    void scrollView(int lines);

    // Inspection (tests, diagnostics).
    QString screenText() const;
    QString lineText(int row) const;
    QColor foregroundAt(int row, int col) const;
    QColor backgroundAt(int row, int col) const;
    QPoint cursor() const { return m_cursor; }
    bool cursorVisible() const { return m_cursorVisible; }
    bool altScreen() const { return m_altScreen; }
    int mouseMode() const { return m_mouseMode; }
    QString title() const { return m_title; }

    qint64 pid() const { return m_pid; }
    bool running() const { return m_pid > 0 && !m_exited; }
    int exitCode() const { return m_exitCode; }
    void terminate();

    static bool available() { return true; }

signals:
    void updated(); // the image (or cursor) changed
    void finished(int exitCode);
    void titleChanged(const QString &title);

private:
    struct Cell
    {
        uint32_t ch[6] = {};
        int width = 1;
        QColor fg, bg; // bg invalid = default (transparent)
        bool bold = false, italic = false, underline = false, strike = false, reverse = false, conceal = false;
    };
    using Line = std::vector<Cell>;

    friend struct Callbacks;
    int onDamage(int r0, int r1, int c0, int c1);
    int onMoveCursor(int row, int col, bool visible);
    int onProp(int prop, const void *value);
    int onPushLine(int cols, const void *cells);
    int onPopLine(int cols, void *cells);
    void onOutput(const char *bytes, size_t len);

    void readPty();
    void flushWrites();
    void checkChild();
    void finish();
    void regrid(int rows, int cols);
    void redraw(int r0, int r1);
    void paintCell(QPainter &p, const Cell &c, int row, int col) const;
    Cell cellFromVTerm(const void *vtermCell) const;
    Cell cellAt(int row, int col) const; // view coordinates (honours scrollback offset)
    QColor resolve(const void *vtermColor, bool fg) const;
    int toVTermMods(Qt::KeyboardModifiers mods) const;
    QPoint cellPos(const QPointF &pos) const;
    void snapToBottom();

    QFont m_font, m_bold, m_italic, m_boldItalic;
    Palette m_palette;
    QSize m_cell;
    int m_rows = 24, m_cols = 80;
    int m_scrollbackMax;
    std::deque<Line> m_scrollback;
    int m_offset = 0;

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    QImage m_image;
    int m_dirtyTop = -1, m_dirtyBottom = -1;

    QPoint m_cursor;
    bool m_cursorVisible = true;
    int m_cursorShape = 1;
    bool m_altScreen = false;
    int m_mouseMode = 0;
    bool m_focused = false;
    QString m_title;

    int m_master = -1;
    qint64 m_pid = -1;
    bool m_exited = false;
    int m_exitCode = -1;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    QByteArray m_pendingWrite;
    QTimer *m_reaper = nullptr;
};

// The shade's palette for terminals (from the theme).
Palette themePalette();

} // namespace shade::term
