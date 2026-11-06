#pragma once

#include <shade/sdk.h>

#include <QColor>
#include <QFont>
#include <QHash>
#include <QString>

namespace shade
{

// Theme colors shared by the host and plugins. The host fills it from the
// [theme] section of config.toml before plugins load; anything not configured
// keeps the built-in dark palette (flexoki-dark inspired).
class SHADE_SDK_EXPORT Theme
{
public:
    static const Theme &get();
    static void set(const Theme &theme);

    Theme();

    // Named color lookup ("bg", "tx-2", "red", ...). Unknown -> magenta.
    QColor color(const QString &name) const;
    void setColor(const QString &name, const QColor &color);
    QStringList names() const;

    QColor bg() const { return color(QStringLiteral("bg")); }     // pill surface
    QColor bg2() const { return color(QStringLiteral("bg-2")); }  // cards
    QColor ui() const { return color(QStringLiteral("ui")); }     // hover
    QColor ui2() const { return color(QStringLiteral("ui-2")); }
    QColor ui3() const { return color(QStringLiteral("ui-3")); }
    QColor tx() const { return color(QStringLiteral("tx")); }     // text
    QColor tx2() const { return color(QStringLiteral("tx-2")); }  // muted text
    QColor tx3() const { return color(QStringLiteral("tx-3")); }  // faint text
    QColor accent() const { return color(QStringLiteral("accent")); }
    QColor red() const { return color(QStringLiteral("red")); }
    QColor green() const { return color(QStringLiteral("green")); }
    QColor yellow() const { return color(QStringLiteral("yellow")); }

    // Font helper: pixel size + weight on top of the application font.
    static QFont font(int pixelSize, QFont::Weight weight = QFont::Normal);

private:
    QHash<QString, QColor> m_colors;
};

// Linear interpolation helpers used by host and plugins alike.
inline qreal lerp(qreal a, qreal b, qreal t) { return a + (b - a) * t; }
SHADE_SDK_EXPORT QColor mix(const QColor &a, const QColor &b, qreal t);
SHADE_SDK_EXPORT QColor withAlpha(const QColor &c, qreal alpha);

} // namespace shade
