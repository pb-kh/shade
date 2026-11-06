// The embedded terminal emulator: pty child, screen model, input encoding,
// resize, scrollback, rendering and process teardown.
#include "term/terminal.h"

#include <QKeyEvent>
#include <QPainter>
#include <QSignalSpy>
#include <QTest>

#include <csignal>
#include <sys/wait.h>

using shade::term::Terminal;

class TestTerminal : public QObject
{
    Q_OBJECT

    static QFont font() { return QFont(QStringLiteral("monospace"), 11); }
    static QStringList sh(const QString &script) { return {QStringLiteral("sh"), QStringLiteral("-c"), script}; }

private slots:
    void runsCommandAndReportsExit()
    {
        Terminal t(font(), shade::term::themePalette());
        QSignalSpy done(&t, &Terminal::finished);
        QVERIFY(t.start(sh(QStringLiteral("printf 'hello\\r\\nworld'; exit 3")), QSize(400, 200)));
        QVERIFY(t.pid() > 0);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
        QCOMPARE(done.first().at(0).toInt(), 3);
        QVERIFY(!t.running());
        QCOMPARE(t.screenText(), QStringLiteral("hello\nworld"));
    }

    void gridFollowsPixels()
    {
        Terminal t(font(), shade::term::themePalette());
        const QSize cell = t.cellSize();
        QVERIFY(cell.width() > 3 && cell.height() > 6);
        QVERIFY(t.start(sh(QStringLiteral("stty size; sleep 0.3; stty size; sleep 5")), QSize(cell.width() * 50 + 3, cell.height() * 10 + 2)));
        QCOMPARE(t.gridSize(), QSize(50, 10));
        QCOMPARE(t.contentSize(), QSize(cell.width() * 50, cell.height() * 10));
        QTRY_VERIFY_WITH_TIMEOUT(t.lineText(0) == QStringLiteral("10 50"), 3000);
        t.setPixelSize(QSize(cell.width() * 30, cell.height() * 8)); // SIGWINCH before the second stty
        QCOMPARE(t.gridSize(), QSize(30, 8));
        QTRY_VERIFY_WITH_TIMEOUT(t.screenText().contains(QStringLiteral("8 30")), 3000);
    }

    void colorsAndAttributes()
    {
        Terminal t(font(), shade::term::themePalette());
        QSignalSpy done(&t, &Terminal::finished);
        QVERIFY(t.start(sh(QStringLiteral("printf '\\033[31mR\\033[0m \\033[38;2;10;20;30;48;5;21mT\\033[0m'")), QSize(300, 100)));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
        QCOMPARE(t.lineText(0), QStringLiteral("R T"));
        QCOMPARE(t.foregroundAt(0, 0), shade::term::themePalette().ansi[1]); // palette red, from the theme
        QCOMPARE(t.foregroundAt(0, 2), QColor(10, 20, 30));                   // truecolor
        QCOMPARE(t.backgroundAt(0, 2), QColor(0, 0, 255));                    // 256-colour cube #21
        QVERIFY(!t.backgroundAt(0, 0).isValid());                             // default bg stays transparent
    }

    void keyboardInput()
    {
        Terminal t(font(), shade::term::themePalette());
        QVERIFY(t.start({QStringLiteral("cat")}, QSize(400, 200)));
        t.sendText(QStringLiteral("abc\n"));
        QTRY_VERIFY_WITH_TIMEOUT(t.screenText() == QStringLiteral("abc\nabc"), 3000); // echo + cat
        // Ctrl+C interrupts cat (ISIG on the pty).
        QSignalSpy done(&t, &Terminal::finished);
        QKeyEvent ctrlC(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03"));
        QVERIFY(t.sendKey(&ctrlC));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
        QCOMPARE(done.first().at(0).toInt(), 128 + SIGINT);
    }

    void specialKeysAreEncoded()
    {
        Terminal t(font(), shade::term::themePalette());
        // od shows the raw bytes the keys produced.
        QVERIFY(t.start(sh(QStringLiteral("stty raw -echo; head -c 7 | od -An -c | tr -s ' '; sleep 5")), QSize(600, 200)));
        QTest::qWait(150);
        QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
        QKeyEvent bs(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QKeyEvent altX(QEvent::KeyPress, Qt::Key_X, Qt::AltModifier, QStringLiteral("x"));
        for (QKeyEvent *e : {&up, &tab, &bs, &altX}) QVERIFY(t.sendKey(e));
        QTRY_VERIFY_WITH_TIMEOUT(t.screenText().contains(QStringLiteral("033 [ A \\t 177 033 x")), 3000);
    }

    void scrollback()
    {
        Terminal t(font(), shade::term::themePalette(), 500);
        QSignalSpy done(&t, &Terminal::finished);
        QVERIFY(t.start(sh(QStringLiteral("seq 1 100")), QSize(t.cellSize().width() * 20, t.cellSize().height() * 10)));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
        QCOMPARE(t.lineText(9), QString()); // the cursor line after "100"
        QCOMPARE(t.lineText(8), QStringLiteral("100"));
        QVERIFY(t.scrollbackLines() >= 90);
        t.wheel(QPointF(5, 5), 2, Qt::NoModifier); // no mouse mode, no alt screen: scroll the view
        QCOMPARE(t.scrollOffset(), 6);
        QCOMPARE(t.lineText(8), QStringLiteral("94"));
        t.scrollView(1000);
        QCOMPARE(t.lineText(0), QStringLiteral("1"));
        t.scrollView(-1000);
        QCOMPARE(t.scrollOffset(), 0);
    }

    void altScreenAndMouseModes()
    {
        Terminal t(font(), shade::term::themePalette());
        QVERIFY(t.start(sh(QStringLiteral("printf '\\033[?1049h\\033[?1000hX'; sleep 5")), QSize(300, 100)));
        QTRY_VERIFY_WITH_TIMEOUT(t.altScreen() && t.mouseMode() != 0, 3000);
        QCOMPARE(t.lineText(0), QStringLiteral("X"));
        t.wheel(QPointF(1, 1), 1, Qt::NoModifier); // reported to the app, not scrolled
        QCOMPARE(t.scrollOffset(), 0);
    }

    void spawnFailureLeavesNothingBehind()
    {
        Terminal t(font(), shade::term::themePalette());
        QString error;
        QVERIFY(!t.start({QStringLiteral("/nonexistent/definitely-not-here")}, QSize(300, 100), &error));
        QVERIFY2(error.contains(QStringLiteral("No such file")), qPrintable(error));
        QCOMPARE(t.pid(), -1);
        QVERIFY(!t.running());
        QVERIFY(!t.start({}, QSize(300, 100), &error));
    }

    void destructionKillsTheProcessGroup()
    {
        qint64 pid = -1, child = -1;
        {
            Terminal t(font(), shade::term::themePalette());
            // A shell that ignores SIGHUP-less exits and has a background child.
            QVERIFY(t.start(sh(QStringLiteral("sleep 100 & echo $!; wait")), QSize(300, 100)));
            pid = t.pid();
            QTRY_VERIFY_WITH_TIMEOUT(!t.lineText(0).isEmpty(), 3000);
            child = t.lineText(0).toLongLong();
            QVERIFY(child > 0);
        }
        QCOMPARE(::kill(pid_t(pid), 0), -1); // reaped
        QTRY_VERIFY_WITH_TIMEOUT(::kill(pid_t(child), 0) == -1, 3000); // the group got SIGHUP
    }

    void terminateIsAsyncAndReported()
    {
        Terminal t(font(), shade::term::themePalette());
        QSignalSpy done(&t, &Terminal::finished);
        QVERIFY(t.start(sh(QStringLiteral("trap '' HUP; sleep 100")), QSize(300, 100))); // ignores SIGHUP
        QTest::qWait(100);
        t.terminate();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000); // escalated to SIGKILL
    }

    void renders()
    {
        Terminal t(font(), shade::term::themePalette());
        QSignalSpy done(&t, &Terminal::finished);
        QVERIFY(t.start(sh(QStringLiteral("printf '\\033[42m    \\033[0m'")), QSize(200, 60)));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
        QImage img(t.contentSize(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        t.paint(&p, QPointF(0, 0));
        p.end();
        QCOMPARE(img.pixelColor(t.cellSize().width() / 2, t.cellSize().height() / 2), shade::term::themePalette().ansi[2]);
        QCOMPARE(img.pixelColor(img.width() - 2, img.height() - 2).alpha(), 0);
    }
};

QTEST_MAIN(TestTerminal)
#include "test_terminal.moc"
