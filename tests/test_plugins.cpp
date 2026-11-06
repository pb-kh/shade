// Unit tests for plugin internals that need no D-Bus services: the
// notification model, album-art resolution/loading, the wired-network sysfs
// probe and the pactl volume source (against tests/integration/fake-pactl).
#include "../plugins/media/art.h"
#include "../plugins/notifications/model.h"
#include "../plugins/stats/sources.h"

#include <QBuffer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class TestPlugins : public QObject
{
    Q_OBJECT

private slots:
    void notificationOrderAndReplace()
    {
        notif::Model m;
        QSignalSpy added(&m, &notif::Model::added), replaced(&m, &notif::Model::replaced), removed(&m, &notif::Model::removed);
        notif::Notification a;
        a.summary = QStringLiteral("a");
        const quint32 ida = m.upsert(a, 0);
        notif::Notification b;
        b.summary = QStringLiteral("b");
        const quint32 idb = m.upsert(b, 0);
        QVERIFY(ida != 0 && idb != ida);
        QCOMPARE(m.items().first().summary, QStringLiteral("b")); // newest first
        notif::Notification a2;
        a2.summary = QStringLiteral("a2");
        QCOMPARE(m.upsert(a2, ida), ida); // replacement keeps id and position
        QCOMPARE(m.items().last().summary, QStringLiteral("a2"));
        QCOMPARE(added.count(), 2);
        QCOMPARE(replaced.count(), 1);
        QVERIFY(!m.remove(999));
        QVERIFY(m.remove(idb));
        QCOMPARE(removed.count(), 1);
        QCOMPARE(m.clear(), QList<quint32>{ida});
        QVERIFY(m.items().isEmpty());
        QVERIFY(m.clear().isEmpty());
    }

    void notificationAge()
    {
        const QDateTime now = QDateTime::currentDateTime();
        QCOMPARE(notif::Model::age(now.addSecs(-5), now), QStringLiteral("now"));
        QCOMPARE(notif::Model::age(now.addSecs(-61), now), QStringLiteral("1m"));
        QCOMPARE(notif::Model::age(now.addSecs(-59 * 60), now), QStringLiteral("59m"));
        QCOMPARE(notif::Model::age(now.addSecs(-3600), now), QStringLiteral("1h"));
        QCOMPARE(notif::Model::age(now.addSecs(-5400), now), QStringLiteral("1.5h"));
        QCOMPARE(notif::Model::age(now.addSecs(-7300), now), QStringLiteral("2h"));
        QCOMPARE(notif::Model::age(now.addSecs(30), now), QStringLiteral("now")); // clock skew
    }

    void notificationBodyTruncation()
    {
        QCOMPARE(notif::Model::truncateBody(QStringLiteral("  short  ")), QStringLiteral("short"));
        const QString longBody(500, u'x');
        const QString t = notif::Model::truncateBody(longBody);
        QCOMPARE(t.size(), 201);
        QVERIFY(t.endsWith(QChar(0x2026)));
    }

    void youtubeFallback()
    {
        using media::ArtLoader;
        QCOMPARE(ArtLoader::youtubeId(QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=3")), QStringLiteral("dQw4w9WgXcQ"));
        QCOMPARE(ArtLoader::youtubeId(QStringLiteral("https://www.youtube.com/watch?list=x&v=dQw4w9WgXcQ")), QStringLiteral("dQw4w9WgXcQ"));
        QCOMPARE(ArtLoader::youtubeId(QStringLiteral("https://youtu.be/dQw4w9WgXcQ")), QStringLiteral("dQw4w9WgXcQ"));
        QCOMPARE(ArtLoader::youtubeId(QStringLiteral("https://music.youtube.com/watch?v=dQw4w9WgXcQ")), QStringLiteral("dQw4w9WgXcQ"));
        QVERIFY(ArtLoader::youtubeId(QStringLiteral("https://example.com/watch?v=dQw4w9WgXcQ")).isEmpty());
        QVERIFY(ArtLoader::youtubeId(QStringLiteral("https://evil.com/?u=https://youtu.be/dQw4w9WgXcQ")).isEmpty());
        QCOMPARE(ArtLoader::resolve(QString(), QStringLiteral("https://youtu.be/dQw4w9WgXcQ")),
                 QStringLiteral("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg"));
        QCOMPARE(ArtLoader::resolve(QStringLiteral("file:///a.png"), QStringLiteral("https://youtu.be/dQw4w9WgXcQ")), QStringLiteral("file:///a.png"));
        QVERIFY(ArtLoader::resolve(QString(), QString()).isEmpty());
    }

    void artFromFileAndData()
    {
        QTemporaryDir dir;
        QImage img(40, 20, QImage::Format_RGB32);
        img.fill(Qt::red);
        const QString path = dir.filePath(QStringLiteral("a.png"));
        QVERIFY(img.save(path));
        media::ArtLoader loader;
        QSignalSpy ready(&loader, &media::ArtLoader::ready);

        loader.load(QUrl::fromLocalFile(path).toString());
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.last().at(1).value<QImage>().size(), QSize(40, 20));

        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        loader.load(QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64()));
        QCOMPARE(ready.last().at(1).value<QImage>().size(), QSize(40, 20));

        loader.load(QStringLiteral("file:///nonexistent/x.png"));
        QVERIFY(ready.last().at(1).value<QImage>().isNull());
        loader.load(QStringLiteral("ftp://example.com/x.png")); // unsupported scheme
        QVERIFY(ready.last().at(1).value<QImage>().isNull());
        loader.load(QString());
        QVERIFY(ready.last().at(1).value<QImage>().isNull());

        const QImage bg = media::blurredBackground(img, QSize(380, 128));
        QCOMPARE(bg.size(), QSize(380, 128));
        QVERIFY(media::blurredBackground(QImage(), QSize(10, 10)).isNull());
    }

    void wiredDetection()
    {
        QTemporaryDir dir;
        auto iface = [&](const QString &name, bool device, bool wireless, const char *type, const char *oper) {
            QDir(dir.path()).mkpath(name);
            const QString base = dir.filePath(name);
            if (device) QDir(base).mkpath(QStringLiteral("device"));
            if (wireless) QDir(base).mkpath(QStringLiteral("wireless"));
            auto write = [&](const char *f, const char *v) {
                QFile file(base + u'/' + QLatin1String(f));
                QVERIFY(file.open(QIODevice::WriteOnly));
                file.write(v);
            };
            write("type", type);
            write("operstate", oper);
            write("carrier", strcmp(oper, "up") == 0 ? "1" : "0");
        };
        iface(QStringLiteral("lo"), false, false, "772", "unknown");
        iface(QStringLiteral("wlan0"), true, true, "1", "up");
        iface(QStringLiteral("docker0"), false, false, "1", "up"); // virtual
        iface(QStringLiteral("eth0"), true, false, "1", "down");
        qputenv("SHADE_SYSFS_NET", dir.path().toLocal8Bit());
        stats::registerDBusTypes();
        stats::Network offline(QDBusConnection(QStringLiteral("none")), QStringLiteral("none"));
        QCOMPARE(offline.kind(), stats::Network::Kind::Offline);
        QVERIFY(offline.icon().endsWith(QLatin1String("network-offline.svg")));

        QFile oper(dir.filePath(QStringLiteral("eth0/operstate")));
        QVERIFY(oper.open(QIODevice::WriteOnly));
        oper.write("up");
        oper.close();
        stats::Network wired(QDBusConnection(QStringLiteral("none")), QStringLiteral("none"));
        QCOMPARE(wired.kind(), stats::Network::Kind::Wired);
        QCOMPARE(wired.describe(), QStringLiteral("Ethernet"));
        qunsetenv("SHADE_SYSFS_NET");
    }

    void volumeViaPactl()
    {
        QTemporaryDir dir;
        const QString state = dir.filePath(QStringLiteral("pactl"));
        qputenv("FAKE_PACTL_STATE", state.toLocal8Bit());
        {
            QFile f(state);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("40 no\n");
        }
        stats::Volume vol(QStringLiteral(SHADE_FAKE_PACTL), 5);
        QSignalSpy events(&vol, &stats::Volume::notable);
        QTRY_VERIFY(vol.available());
        QCOMPARE(vol.level(), 40);
        QVERIFY(!vol.muted());
        QVERIFY(vol.icon().endsWith(QLatin1String("volume-2.svg")));

        // Our own change: applied optimistically, no peek event.
        vol.adjust(2);
        QCOMPARE(vol.level(), 50);
        auto read = [&] {
            QFile f(state);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
        };
        QTRY_COMPARE_WITH_TIMEOUT(read(), QStringLiteral("50 no"), 3000);
        QTest::qWait(400);
        QCOMPARE(events.count(), 0);
        vol.adjust(100);
        QCOMPARE(vol.level(), 100); // clamped

        // An external change (another app) becomes a peek event.
        QTest::qWait(1700); // past the own-change window
        {
            QFile f(state);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("10 yes\n");
        }
        QTRY_VERIFY_WITH_TIMEOUT(events.count() >= 1, 3000);
        QVERIFY(vol.muted());
        QCOMPARE(vol.level(), 10);
        QVERIFY(vol.icon().endsWith(QLatin1String("volume-muted.svg")));
        const auto e = events.last().at(0).value<stats::Event>();
        QCOMPARE(e.kind, QStringLiteral("volume"));
        QCOMPARE(e.text, QStringLiteral("Muted"));
    }

    void volumeUnavailable()
    {
        stats::Volume vol(QStringLiteral("/nonexistent/pactl"), 5);
        QTest::qWait(200);
        QVERIFY(!vol.available());
        vol.adjust(1); // no-op, no crash
        QVERIFY(vol.icon().endsWith(QLatin1String("volume-muted.svg")));
    }
};

QTEST_GUILESS_MAIN(TestPlugins)
#include "test_plugins.moc"
