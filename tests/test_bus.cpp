#include "core/signalbus.h"

#include <QSignalSpy>
#include <QTest>

using namespace shade;
using namespace shade::core;

class TestBus : public QObject
{
    Q_OBJECT

private slots:
    void splitCommandQuoting()
    {
        auto split = [](const QString &c) {
            QStringList out;
            return shade::splitCommand(c, &out) ? out : QStringList{QStringLiteral("<error>")};
        };
        QCOMPARE(split(QStringLiteral("iwctl")), QStringList{QStringLiteral("iwctl")});
        QCOMPARE(split(QStringLiteral("  sh  -c 'echo a; exec cat'  ")),
                 (QStringList{QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("echo a; exec cat")}));
        QCOMPARE(split(QStringLiteral("a \"b \\\"c\\\" \\$x \\n\" d\\ e '' \"\"")),
                 (QStringList{QStringLiteral("a"), QStringLiteral("b \"c\" $x \\n"), QStringLiteral("d e"), QString(), QString()}));
        QCOMPARE(split(QStringLiteral("it's'")), QStringList{QStringLiteral("its")}); // quotes concatenate
        QCOMPARE(split(QStringLiteral("$HOME *")), (QStringList{QStringLiteral("$HOME"), QStringLiteral("*")})); // no expansion
        QCOMPARE(split(QStringLiteral("'open")), QStringList{QStringLiteral("<error>")});
        QCOMPARE(split(QStringLiteral("trail\\")), QStringList{QStringLiteral("<error>")});
        QCOMPARE(split(QString()), QStringList{});
    }

    void routesOnlyToDestination()
    {
        SignalBus bus;
        auto *host = bus.endpoint(QStringLiteral("shade"));
        auto *a = bus.endpoint(QStringLiteral("a"));
        auto *b = bus.endpoint(QStringLiteral("b"));
        int ga = 0, gb = 0, gh = 0;
        a->handle(QStringLiteral("ping"), [&](const Signal &s) { ++ga; QCOMPARE(s.source, QStringLiteral("b")); });
        b->handle(QStringLiteral("ping"), [&](const Signal &) { ++gb; });
        host->handle(QStringLiteral("ping"), [&](const Signal &) { ++gh; });
        QVERIFY(b->send(QStringLiteral("a"), QStringLiteral("ping")));
        QCOMPARE(ga, 0); // deferred
        QCoreApplication::processEvents();
        QCOMPARE(ga, 1);
        QCOMPARE(gb, 0);
        QCOMPARE(gh, 0);
    }

    void broadcastSkipsHostAndSender()
    {
        SignalBus bus;
        auto *host = bus.endpoint(QStringLiteral("shade"));
        auto *a = bus.endpoint(QStringLiteral("a"));
        auto *b = bus.endpoint(QStringLiteral("b"));
        auto *c = bus.endpoint(QStringLiteral("c"));
        int n = 0, nh = 0, na = 0;
        for (auto *e : {b, c}) e->handle(QStringLiteral("hello"), [&](const Signal &) { ++n; });
        a->handle(QStringLiteral("hello"), [&](const Signal &) { ++na; });
        host->handle(QStringLiteral("hello"), [&](const Signal &) { ++nh; });
        a->send(QStringLiteral("*"), QStringLiteral("hello"));
        QCoreApplication::processEvents();
        QCOMPARE(n, 2);
        QCOMPARE(na, 0);
        QCOMPARE(nh, 0);
    }

    void rejectsForgedAndMalformed()
    {
        SignalBus bus;
        bus.endpoint(QStringLiteral("shade"));
        auto *a = bus.endpoint(QStringLiteral("a"));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("forged")));
        QVERIFY(!a->send(Signal{QStringLiteral("x"), QStringLiteral("shade"), QStringLiteral("shade"), {}, {}}));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("unknown destination")));
        QVERIFY(!a->send(QStringLiteral("ghost"), QStringLiteral("x")));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("bad name")));
        QVERIFY(!a->send(QStringLiteral("shade"), QStringLiteral("not a name!")));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("missing destination")));
        QVERIFY(!a->send(QString(), QStringLiteral("x")));
        QCOMPARE(bus.diagnostics().size(), 4);
    }

    void throwingHandlerIsContained()
    {
        SignalBus bus;
        auto *a = bus.endpoint(QStringLiteral("a"));
        auto *b = bus.endpoint(QStringLiteral("b"));
        int after = 0;
        a->handle(QStringLiteral("x"), [](const Signal &) { throw std::runtime_error("boom"); });
        a->handle(QStringLiteral("x"), [&](const Signal &) { ++after; });
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("threw")));
        b->send(QStringLiteral("a"), QStringLiteral("x"));
        QCoreApplication::processEvents();
        QCOMPARE(after, 1);
    }

    void removedEndpointDropsQueued()
    {
        SignalBus bus;
        auto *a = bus.endpoint(QStringLiteral("a"));
        auto *b = bus.endpoint(QStringLiteral("b"));
        int got = 0;
        a->handle(QStringLiteral("x"), [&](const Signal &) { ++got; });
        b->send(QStringLiteral("a"), QStringLiteral("x"));
        bus.removeEndpoint(QStringLiteral("a"));
        QCoreApplication::processEvents();
        QCOMPARE(got, 0);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("unknown destination")));
        QVERIFY(!b->send(QStringLiteral("a"), QStringLiteral("x")));
    }

    void unhandle()
    {
        SignalBus bus;
        auto *a = bus.endpoint(QStringLiteral("a"));
        int got = 0;
        const int id = a->handle(QStringLiteral("x"), [&](const Signal &) { ++got; });
        a->unhandle(id);
        QCOMPARE(a->handlerCount(), 0);
        a->send(QStringLiteral("a"), QStringLiteral("x"));
        QCoreApplication::processEvents();
        QCOMPARE(got, 0);
    }
};

QTEST_GUILESS_MAIN(TestBus)
#include "test_bus.moc"
