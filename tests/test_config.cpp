#include "core/config.h"

#include <QTemporaryDir>
#include <QTest>

#include "test_data.h"

using namespace shade::core;

class TestConfig : public QObject
{
    Q_OBJECT

private slots:
    void exampleConfigIsClean()
    {
        // The shipped example must parse with no warnings and give the
        // built-in defaults.
        const Config c = Config::load(QStringLiteral(SHADE_EXAMPLE_CONFIG));
        QVERIFY2(c.warnings.isEmpty(), qPrintable(c.warnings.join(u'\n')));
        const Settings defaults;
        QCOMPARE(c.settings.barWidth, defaults.barWidth);
        QCOMPARE(c.settings.peekTimeoutMs, defaults.peekTimeoutMs);
        QCOMPARE(c.settings.expandDamping, defaults.expandDamping);
        const QVariantMap stats = c.pluginSection(QStringLiteral("stats"));
        QCOMPARE(stats.value(QStringLiteral("network_backend")).toString(), QStringLiteral("auto"));
        QVERIFY(stats.value(QStringLiteral("peeks")).canConvert<QVariantMap>());
        QCOMPARE(c.pluginSection(QStringLiteral("notifications")).value(QStringLiteral("peek_timeout_ms")).toInt(), 5000);
    }

    void missingFileIsDefaults()
    {
        const Config c = Config::load(QStringLiteral("/nonexistent/shade/config.toml"));
        QVERIFY(c.warnings.isEmpty());
        QCOMPARE(c.settings.barWidth, 300.0);
    }

    void emptyIsDefaults()
    {
        const Config c = Config::parse("  \n");
        QVERIFY(c.warnings.isEmpty());
    }

    void parsesSectionsAndPlugins()
    {
        const Config c = Config::parse(kConfig1);
        QVERIFY2(c.warnings.isEmpty(), qPrintable(c.warnings.join(u'\n')));
        QCOMPARE(c.settings.barWidth, 240.0);
        QCOMPARE(c.settings.peekTimeoutMs, 3000);
        QCOMPARE(c.settings.cornerRadius, 12.5);
        QCOMPARE(c.settings.terminal, QStringLiteral("foot"));
        QCOMPARE(c.settings.disabledPlugins, QStringList{QStringLiteral("media")});
        QCOMPARE(c.theme.bg(), QColor(QStringLiteral("#101010")));
        QCOMPARE(c.theme.accent(), QColor(QStringLiteral("#ff8800")));
        QCOMPARE(c.theme.tx(), shade::Theme().tx()); // untouched default
        const QVariantMap stats = c.pluginSection(QStringLiteral("stats"));
        QCOMPARE(stats.value(QStringLiteral("network_backend")).toString(), QStringLiteral("iwd"));
        QCOMPARE(stats.value(QStringLiteral("battery_low")).toList().size(), 3);
        QCOMPARE(stats.value(QStringLiteral("peeks")).toMap().value(QStringLiteral("volume")).toBool(), false);
        QVERIFY(c.pluginSection(QStringLiteral("absent")).isEmpty());
    }

    void wrongTypesWarnAndKeepDefaults()
    {
        const Config c = Config::parse(kConfig2, QStringLiteral("/x/config.toml"));
        QCOMPARE(c.settings.barWidth, 300.0);
        QCOMPARE(c.settings.idleTimeoutMs, 0);
        QCOMPARE(c.warnings.size(), 4);
        for (const QString &w : c.warnings) QVERIFY(w.startsWith(QStringLiteral("/x/config.toml")));
    }

    void syntaxErrorWarns()
    {
        const Config c = Config::parse("[shade\nbar_width = ", QStringLiteral("/x/config.toml"));
        QCOMPARE(c.warnings.size(), 1);
        QVERIFY(c.warnings[0].contains(QStringLiteral("/x/config.toml:")));
        QCOMPARE(c.settings.barWidth, 300.0);
    }
};

QTEST_GUILESS_MAIN(TestConfig)
#include "test_config.moc"
