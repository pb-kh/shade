#include "base/pidlock.h"
#include "core/control.h"

#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

using namespace shade;

class TestBase : public QObject
{
    Q_OBJECT

private slots:
    void parsePidIsStrict()
    {
        QCOMPARE(base::parsePid("1234\n"), pid_t(1234));
        QCOMPARE(base::parsePid("0"), pid_t(0));
        QCOMPARE(base::parsePid("1"), pid_t(0));
        QCOMPARE(base::parsePid("-1"), pid_t(0));
        QCOMPARE(base::parsePid("12ab"), pid_t(0));
        QCOMPARE(base::parsePid(" 99999999999 "), pid_t(0));
        QCOMPARE(base::parsePid(""), pid_t(0));
    }

    void lockLifecycle()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("shade.pid"));
        {
            base::PidLock lock(path);
            QCOMPARE(lock.acquire(), base::PidLock::Acquired);
            QCOMPARE(base::readPid(path), getpid());
        }
        QVERIFY(!QFile::exists(path));
    }

    void staleLockIsReplaced()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("shade.pid"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("999999\n"); // no such process
        f.close();
        base::PidLock lock(path);
        QCOMPARE(lock.acquire(), base::PidLock::Acquired);
    }

    void liveLockRefuses()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("shade.pid"));
        const pid_t child = fork();
        if (child == 0) {
            sleep(5);
            _exit(0);
        }
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::number(child));
        f.close();
        base::PidLock lock(path);
        QCOMPARE(lock.acquire(), base::PidLock::AlreadyRunning);
        QCOMPARE(lock.runningPid(), child);
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        QVERIFY(!base::pidAlive(child));
        QCOMPARE(lock.acquire(), base::PidLock::Acquired);
    }

    void nonRegularFileIsError()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("shade.pid"));
        QVERIFY(QDir().mkdir(path));
        base::PidLock lock(path);
        QCOMPARE(lock.acquire(), base::PidLock::Error);
        QVERIFY(lock.error().contains(QStringLiteral("not a regular file")));
    }

    void controlProtocol()
    {
        core::ControlServer server([](const QJsonObject &req) {
            if (req.value(QStringLiteral("cmd")).toString() == QLatin1String("ping")) return QJsonObject{{QStringLiteral("pong"), true}};
            return core::errorReply(QStringLiteral("unknown command"));
        });
        QCOMPARE(server.process("{\"cmd\":\"ping\"}").value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(server.process("not json").value(QStringLiteral("ok")).toBool(), false);
        QCOMPARE(server.process("[1,2]").value(QStringLiteral("ok")).toBool(), false);
        QCOMPARE(server.process("{\"x\":1}").value(QStringLiteral("error")).toString(), QStringLiteral("missing \"cmd\""));
        QCOMPARE(server.process(QByteArray(core::ControlServer::kMaxRequest + 1, 'a')).value(QStringLiteral("ok")).toBool(), false);

        QTemporaryDir dir;
        const QString sock = dir.filePath(QStringLiteral("s.sock"));
        QVERIFY(server.listen(sock));
        QLocalSocket a, b;
        a.connectToServer(sock);
        b.connectToServer(sock);
        QVERIFY(a.waitForConnected(1000));
        QVERIFY(b.waitForConnected(1000));
        a.write("{\"cmd\":\"ping\"}\n{\"cmd\":\"x\"}\n");
        b.write("{\"cmd\":\"ping\"}\n");
        QTRY_VERIFY(a.canReadLine());
        QTRY_VERIFY(b.canReadLine());
        QCOMPARE(QJsonDocument::fromJson(b.readLine()).object().value(QStringLiteral("pong")).toBool(), true);
        QCOMPARE(QJsonDocument::fromJson(a.readLine()).object().value(QStringLiteral("ok")).toBool(), true);
        QTRY_VERIFY(a.canReadLine());
        QCOMPARE(QJsonDocument::fromJson(a.readLine()).object().value(QStringLiteral("ok")).toBool(), false);
        server.close();
        QVERIFY(!QFile::exists(sock));
        // A stale socket file from an unclean exit is replaced.
        QFile stale(sock);
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.close();
        QVERIFY(server.listen(sock));
    }
};

QTEST_GUILESS_MAIN(TestBase)
#include "test_base.moc"
