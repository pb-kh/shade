#include "core/model.h"

#include <QSignalSpy>
#include <QTest>

using namespace shade::core;

class TestModel : public QObject
{
    Q_OBJECT

    Settings s;

    QVector<PaneInfo> panes3() const
    {
        return {{QStringLiteral("a"), QSizeF(200, 60)}, {QStringLiteral("b"), QSizeF(300, 80)}, {QStringLiteral("c"), QSizeF(260, 50)}};
    }

    static void settle(ShadeModel &m, qreal seconds = 3.0)
    {
        for (int i = 0; i < int(seconds * 60); ++i) m.tick(1.0 / 60);
    }

    // One continuous horizontal gesture of `total` px in `steps` events.
    static void swipe(ShadeModel &m, qreal total, int steps = 20)
    {
        for (int i = 0; i < steps; ++i) {
            m.wheel(total / steps, 0);
            m.tick(1.0 / 120);
        }
        m.wheel(0, 0, true);
    }

private slots:
    void startsAsBar()
    {
        ShadeModel m(&s);
        QCOMPARE(m.state(), ShadeModel::State::Bar);
        QCOMPARE(m.pillRect().width(), s.barWidth);
        QCOMPARE(m.pillRect().height(), s.barHeight);
        QVERIFY(!m.needsTick());
    }

    void hoverExpandsAndBounces()
    {
        ShadeModel m(&s);
        m.setCanvasWidth(600);
        m.setPanes(panes3());
        m.pointerMoved(QPointF(300, 2));
        QCOMPARE(m.state(), ShadeModel::State::Widgets);
        qreal maxH = 0;
        for (int i = 0; i < 120; ++i) {
            m.tick(1.0 / 60);
            maxH = std::max(maxH, m.pillRect().height());
        }
        QVERIFY2(maxH > 60.5, "expanding overshoots (bounce)");
        QVERIFY(maxH < 60 * 1.3);
        settle(m);
        QCOMPARE(m.pillRect().size(), QSizeF(200, 60));
        QCOMPARE(m.pillRect().top(), s.topOffset);
        QCOMPARE(m.activePlugin(), QStringLiteral("a"));
    }

    void hoverOutsideBarDoesNothing()
    {
        ShadeModel m(&s);
        m.setCanvasWidth(1000);
        m.setPanes(panes3());
        m.pointerMoved(QPointF(20, 2));
        QCOMPARE(m.state(), ShadeModel::State::Bar);
        m.pointerMoved(QPointF(500, 40));
        QCOMPARE(m.state(), ShadeModel::State::Bar);
    }

    void oneSectionPerGesture()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.expand();
        settle(m);
        swipe(m, 2000); // a huge swipe still moves exactly one pane
        settle(m);
        QCOMPARE(m.selected(), 1);
        QCOMPARE(m.scrollPosition(), m.pager().centers()[1]);
        swipe(m, 2000);
        settle(m);
        QCOMPARE(m.selected(), 2);
        swipe(m, 2000); // clamped at the last pane
        settle(m);
        QCOMPARE(m.selected(), 2);
        swipe(m, -5000);
        settle(m);
        QCOMPARE(m.selected(), 1);
    }

    void rubberBandShowsOnlyALittleMore()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.expand();
        settle(m);
        for (int i = 0; i < 100; ++i) {
            m.wheel(40, 0);
            m.tick(1.0 / 240);
        }
        const qreal next = m.pager().centers()[1];
        QVERIFY(m.scrollPosition() > next);
        QVERIFY2(m.scrollPosition() <= next + s.overscroll + 0.01, "overscroll capped");
    }

    void smallPullSnapsBack()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.expand();
        settle(m);
        // Slow, short pull: below the advance fraction, no flick.
        for (int i = 0; i < 5; ++i) {
            m.wheel(2, 0);
            m.tick(0.04);
        }
        settle(m);
        QCOMPARE(m.selected(), 0);
    }

    void snapBouncesSize()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.expand();
        settle(m);
        swipe(m, 400, 8);
        // The strip overshoots the target pane and comes back; the size,
        // derived from the strip position, bounces with it.
        const qreal target = m.pager().centers()[1];
        qreal maxPos = 0, minW = 1e9;
        for (int i = 0; i < 180; ++i) {
            m.tick(1.0 / 60);
            maxPos = std::max(maxPos, m.scrollPosition());
            minW = std::min(minW, m.pillRect().width());
        }
        QVERIFY2(maxPos > target + 0.5, "strip overshoots");
        QVERIFY2(maxPos < target + 60, "but only a little");
        QVERIFY2(minW < 299.5, "size bounces with the strip");
        QCOMPARE(m.pillRect().width(), 300.0);
    }

    void verticalWheelGoesToPane()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.expand();
        settle(m);
        QVERIFY(!m.wheel(0, 30));
        QCOMPARE(m.selected(), 0);
    }

    void leaveCollapses()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.expand();
        settle(m);
        m.pointerLeft();
        QCOMPARE(m.state(), ShadeModel::State::Bar);
        settle(m);
        QCOMPARE(m.pillRect().height(), s.barHeight);
        QVERIFY(!m.needsTick());
    }

    PeekEntry peek(const QString &plugin, const QString &id, int prio = 2, Expand e = Expand::Hover, int timeout = 0)
    {
        PeekEntry p;
        p.plugin = plugin;
        p.id = id;
        p.priority = prio;
        p.expand = e;
        p.timeoutMs = timeout;
        p.size = QSizeF(180, 34);
        return p;
    }

    void peekFromBarAndTimeout()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        QSignalSpy removed(&m, &ShadeModel::peekRemoved);
        m.publishPeek(peek(QStringLiteral("b"), QStringLiteral("x"), 2, Expand::Hover, 1000));
        QCOMPARE(m.state(), ShadeModel::State::Peek);
        settle(m, 0.9);
        QCOMPARE(m.pillRect().size(), QSizeF(180, 34));
        QCOMPARE(m.morph(), 0.0);
        settle(m, 0.3);
        QCOMPARE(m.state(), ShadeModel::State::Bar);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(2).toString(), QStringLiteral("timeout"));
    }

    void hoverPausesTimeout()
    {
        ShadeModel m(&s);
        m.setCanvasWidth(600);
        m.setPanes(panes3());
        m.publishPeek(peek(QStringLiteral("b"), QStringLiteral("x"), 2, Expand::Top, 500));
        settle(m, 0.3);
        m.pointerMoved(QPointF(300, 25)); // inside, below the reveal zone
        QVERIFY(m.peekHovered());
        settle(m, 2.0);
        QCOMPARE(m.state(), ShadeModel::State::Peek);
        m.pointerLeft();
        settle(m, 1.0);
        QCOMPARE(m.state(), ShadeModel::State::Bar);
    }

    void hoverPeekExpandsIntoOwnerPane()
    {
        ShadeModel m(&s);
        m.setCanvasWidth(600);
        m.setPanes(panes3());
        m.publishPeek(peek(QStringLiteral("c"), QStringLiteral("track")));
        settle(m, 0.5);
        m.pointerMoved(QPointF(300, 25));
        QCOMPARE(m.state(), ShadeModel::State::Widgets);
        QCOMPARE(m.selectedPlugin(), QStringLiteral("c"));
        settle(m);
        QCOMPARE(m.morph(), 1.0);
        QCOMPARE(m.pillRect().size(), QSizeF(260, 50));
        // Leaving returns to the (still queued) peek, not the bar.
        m.pointerLeft();
        QCOMPARE(m.state(), ShadeModel::State::Peek);
        settle(m);
        QCOMPARE(m.morph(), 0.0);
        QCOMPARE(m.pillRect().size(), QSizeF(180, 34));
    }

    void topPeekIgnoresHoverButRevealsAtTop()
    {
        ShadeModel m(&s);
        m.setCanvasWidth(600);
        m.setPanes(panes3());
        m.publishPeek(peek(QStringLiteral("a"), QStringLiteral("n1"), 2, Expand::Top));
        settle(m, 0.5);
        m.pointerMoved(QPointF(300, 25));
        QCOMPARE(m.state(), ShadeModel::State::Peek);
        m.pointerMoved(QPointF(300, 1));
        QCOMPARE(m.state(), ShadeModel::State::Widgets);
        QCOMPARE(m.selectedPlugin(), QStringLiteral("a"));
    }

    void priorityDisplacesAndMorphs()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.publishPeek(peek(QStringLiteral("a"), QStringLiteral("low"), 1));
        settle(m, 0.5);
        auto hi = peek(QStringLiteral("b"), QStringLiteral("hi"), 3);
        hi.size = QSizeF(240, 40);
        m.publishPeek(hi);
        QCOMPARE(m.presentedPeek()->id, QStringLiteral("hi"));
        QVERIFY(m.outgoingPeek());
        QCOMPARE(m.outgoingPeek()->id, QStringLiteral("low"));
        // The bar never appears during a morph.
        for (int i = 0; i < 60; ++i) {
            m.tick(1.0 / 60);
            QVERIFY(m.pillRect().height() >= 33);
        }
        QCOMPARE(m.pillRect().size(), QSizeF(240, 40));
        QVERIFY(!m.outgoingPeek());
        QCOMPARE(m.queue().size(), 2);
    }

    void scrollUpRetiresOnePerGesture()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.publishPeek(peek(QStringLiteral("a"), QStringLiteral("1")));
        m.publishPeek(peek(QStringLiteral("a"), QStringLiteral("2")));
        m.publishPeek(peek(QStringLiteral("a"), QStringLiteral("3")));
        settle(m, 0.5);
        for (int i = 0; i < 20; ++i) {
            m.wheel(0, 20);
            m.tick(0.01);
        }
        QCOMPARE(m.queue().size(), 2);
        m.wheel(0, 0, true);
        for (int i = 0; i < 5; ++i) m.wheel(0, 20);
        QCOMPARE(m.queue().size(), 1);
        m.wheel(0, 0, true);
        for (int i = 0; i < 5; ++i) m.wheel(0, 20);
        QCOMPARE(m.state(), ShadeModel::State::Bar);
    }

    void peeksWaitWhileExpanded()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.expand();
        settle(m);
        m.publishPeek(peek(QStringLiteral("a"), QStringLiteral("1")));
        QCOMPARE(m.state(), ShadeModel::State::Widgets);
        m.pointerLeft();
        QCOMPARE(m.state(), ShadeModel::State::Peek);
    }

    void republishReplacesInPlace()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.publishPeek(peek(QStringLiteral("a"), QStringLiteral("v")));
        auto p = peek(QStringLiteral("a"), QStringLiteral("v"));
        p.data.insert(QStringLiteral("text"), QStringLiteral("new"));
        m.publishPeek(p);
        QCOMPARE(m.queue().size(), 1);
        QCOMPARE(m.presentedPeek()->data.value(QStringLiteral("text")).toString(), QStringLiteral("new"));
        QVERIFY(!m.outgoingPeek());
    }

    void dismissAllOfPlugin()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.publishPeek(peek(QStringLiteral("a"), QStringLiteral("1")));
        m.publishPeek(peek(QStringLiteral("b"), QStringLiteral("2")));
        m.publishPeek(peek(QStringLiteral("a"), QStringLiteral("3")));
        m.dismissPeek(QStringLiteral("a"), QString());
        QCOMPARE(m.queue().size(), 1);
        QCOMPARE(m.presentedPeek()->plugin, QStringLiteral("b"));
    }

    void focusRequest()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        QVERIFY(!m.focusPlugin(QStringLiteral("nope")));
        QCOMPARE(m.state(), ShadeModel::State::Bar);
        QVERIFY(m.focusPlugin(QStringLiteral("c")));
        QCOMPARE(m.state(), ShadeModel::State::Widgets);
        QCOMPARE(m.selected(), 2);
        settle(m);
        QCOMPARE(m.pillRect().size(), QSizeF(260, 50));
        QVERIFY(m.focusPlugin(QStringLiteral("a")));
        settle(m);
        QCOMPARE(m.selected(), 0);
    }

    void activeFollowsSelection()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        QSignalSpy spy(&m, &ShadeModel::activePluginChanged);
        m.expand();
        settle(m);
        swipe(m, 500);
        settle(m);
        m.pointerLeft();
        QCOMPARE(spy.count(), 3);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("a"));
        QCOMPARE(spy.at(1).at(0).toString(), QStringLiteral("b"));
        QCOMPARE(spy.at(2).at(0).toString(), QString());
    }

    void interruptibleExpandCollapse()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.expand();
        for (int i = 0; i < 4; ++i) m.tick(1.0 / 60);
        const qreal mid = m.pillRect().height();
        m.collapse();
        m.tick(1.0 / 60);
        // Continues smoothly from where it was, no jump.
        QVERIFY(std::abs(m.pillRect().height() - mid) < 15);
        settle(m);
        QCOMPARE(m.state(), ShadeModel::State::Bar);
        QCOMPARE(m.pillRect().height(), 1.0);
    }

    void paneResizeKeepsSelection()
    {
        ShadeModel m(&s);
        m.setPanes(panes3());
        m.focusPlugin(QStringLiteral("b"));
        settle(m);
        auto p = panes3();
        p[1].size = QSizeF(340, 120);
        m.setPanes(p);
        settle(m);
        QCOMPARE(m.selectedPlugin(), QStringLiteral("b"));
        QCOMPARE(m.pillRect().size(), QSizeF(340, 120));
    }
};

QTEST_GUILESS_MAIN(TestModel)
#include "test_model.moc"
