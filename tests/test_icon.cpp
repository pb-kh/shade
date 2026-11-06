#include <shade/icon.h>

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTest>

#include "test_data.h"

using namespace shade;

class TestIcon : public QObject
{
    Q_OBJECT

private slots:
    void parsesPathCommands()
    {
        const VectorIcon icon = VectorIcon::fromSvg(kIcon1);
        QCOMPARE(icon.viewBox, QRectF(0, 0, 16, 16));
        QCOMPARE(icon.contours.size(), 3);
        QCOMPARE(icon.contours[0].key, QStringLiteral("p"));
        QCOMPARE(icon.contours[1].key, QStringLiteral("p#1"));
        QVERIFY(icon.contours[0].closed);
        QVERIFY(icon.contours[0].fillOpacity > 0);
        QCOMPARE(icon.contours[1].path.boundingRect(), QRectF(9, 1, 4, 4));
        QCOMPARE(icon.contours[2].strokeWidth, 1.5);
        QCOMPARE(icon.contours[2].fillOpacity, 0.0);
        QCOMPARE(icon.contours[2].cap, Qt::RoundCap);
        QVERIFY(!icon.contours[2].closed);
        QCOMPARE(icon.contours[2].points.size(), VectorIcon::kSamples);
    }

    void shapesAndGroups()
    {
        const VectorIcon icon = VectorIcon::fromSvg(kIcon2);
        QCOMPARE(icon.viewBox, QRectF(0, 0, 20, 10));
        QCOMPARE(icon.contours.size(), 4);
        QCOMPARE(icon.contours[0].path.boundingRect().center(), QPointF(12, 5));
        QCOMPARE(icon.contours[0].strokeOpacity, 0.5);
        QCOMPARE(icon.contours[1].colorRole, QStringLiteral("red"));
        QCOMPARE(icon.contours[3].anchor, QPointF(2, 1));
    }

    void invalidSvgIsNull()
    {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("invalid SVG")));
        QVERIFY(VectorIcon::fromSvg("<svg><path d='M0 0'", QStringLiteral("x")).isNull());
    }

    void morphEndpointsAndMidpoint()
    {
        const VectorIcon a = VectorIcon::fromSvg(kIcon3);
        const VectorIcon b = VectorIcon::fromSvg(kIcon4);
        const VectorIcon mid = interpolateIcons(a, b, 0.5);
        QCOMPARE(mid.contours.size(), 2);
        const IconContour &line = mid.contours[0];
        QCOMPARE(line.key, QStringLiteral("s"));
        for (const QPointF &p : line.points) QVERIFY(std::abs(p.y() - 4) < 1e-6);
        // The appearing dot grows out of its anchor (its centroid).
        const IconContour &dot = mid.contours[1];
        QCOMPARE(dot.key, QStringLiteral("dot"));
        QVERIFY(dot.fillOpacity > 0.4 && dot.fillOpacity < 0.6);
        qreal maxR = 0;
        for (const QPointF &p : dot.points) maxR = std::max(maxR, QLineF(p, QPointF(8, 12)).length());
        QVERIFY(maxR > 0.8 && maxR < 1.2);
        QCOMPARE(interpolateIcons(a, b, 0).contours.size(), 1);
        QCOMPARE(interpolateIcons(a, b, 1).contours.size(), 2);
    }

    void closedAlignmentAvoidsTwisting()
    {
        // Same square, different start points: midpoint stays a square.
        const VectorIcon a = VectorIcon::fromSvg(kIcon5);
        const VectorIcon b = VectorIcon::fromSvg(kIcon6);
        const VectorIcon mid = interpolateIcons(a, b, 0.5);
        for (int i = 0; i < mid.contours[0].points.size(); ++i) {
            const QPointF d = mid.contours[0].points[i] - a.contours[0].points[i];
            QVERIFY(std::hypot(d.x(), d.y()) < 0.5);
        }
    }

    void bundledIconsParse_data()
    {
        QTest::addColumn<QString>("dir");
        QTest::newRow("all") << QStringLiteral(SHADE_ICON_DIRS);
    }

    void bundledIconsParse()
    {
        QFETCH(QString, dir);
        int count = 0;
        for (const QString &d : dir.split(u':')) {
            for (const QFileInfo &fi : QDir(d).entryInfoList({QStringLiteral("*.svg")})) {
                QFile f(fi.absoluteFilePath());
                QVERIFY(f.open(QIODevice::ReadOnly));
                const VectorIcon icon = VectorIcon::fromSvg(f.readAll(), fi.fileName());
                QVERIFY2(!icon.isNull(), qPrintable(fi.fileName()));
                QImage img(32, 32, QImage::Format_ARGB32_Premultiplied);
                img.fill(Qt::transparent);
                QPainter p(&img);
                paintIcon(&p, icon, QRectF(0, 0, 32, 32), Qt::white);
                p.end();
                int lit = 0;
                for (int y = 0; y < 32; ++y)
                    for (int x = 0; x < 32; ++x) lit += qAlpha(img.pixel(x, y)) > 0;
                QVERIFY2(lit > 10, qPrintable(fi.fileName() + QStringLiteral(" renders")));
                ++count;
            }
        }
        QVERIFY(count > 10);
    }

    void morphIconAnimates()
    {
        const QString dir = QStringLiteral(SHADE_ICON_DIRS).split(u':').first();
        MorphIcon icon;
        icon.setDuration(100);
        const QStringList files = QDir(dir).entryList({QStringLiteral("*.svg")});
        QVERIFY(files.size() >= 2);
        icon.setSource(dir + u'/' + files[0], false);
        QVERIFY(!icon.isAnimating());
        icon.setSource(dir + u'/' + files[1]);
        QVERIFY(icon.isAnimating());
        QTRY_VERIFY_WITH_TIMEOUT(!icon.isAnimating(), 1000);
    }
};

QTEST_MAIN(TestIcon)
#include "test_icon.moc"
