#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace shade::core
{

// Core [shade] settings. Defaults match the built-in behaviour; config.toml
// overrides individual keys (see docs/config.md).
struct Settings
{
    // Geometry.
    qreal barWidth = 300.0;       // idle bar
    qreal barHeight = 1.0;
    qreal topOffset = 10.0;       // gap between the screen top and the pill
    qreal cornerRadius = 18.0;    // max pill corner radius
    qreal hoverZone = 16.0;       // height of the bar's hover strip
    qreal revealZone = 3.0;       // "cursor at the very top" for top-expanding peeks
    qreal sectionGap = 36.0;      // horizontal gap between panes on the strip

    // Paging (one pane per gesture, rubber band past it).
    qreal scrollDirection = 1.0;  // -1 flips horizontal direction (mouse wheels)
    qreal touchpadScrollDirection = -1.0; // touchpad swipes page the other way
    qreal stickyDistance = 28.0;  // finger travel before the strip breaks free
    qreal stickyResistance = 0.22;
    qreal overscroll = 36.0;      // max rubber-band travel past the next pane
    qreal overscrollResistance = 0.6;
    qreal advanceFraction = 0.25; // pull this far to advance on release
    qreal flickSpeed = 80.0;      // px/s flick advances regardless of distance
    int scrollReleaseMs = 50;     // idle time that ends a gesture
    qreal pixelsPerWheelNotch = 40.0;
    qreal peekDismissDistance = 24.0; // scroll-up travel that retires a peek

    // Springs (stiffness in 1/s^2, damping ratio).
    qreal expandStiffness = 380.0;
    qreal expandDamping = 0.62;   // < 1 overshoots: the expand bounce
    qreal collapseStiffness = 520.0;
    qreal collapseDamping = 0.9;
    qreal scrollStiffness = 300.0;
    qreal scrollDamping = 0.68;   // the snap bounce
    qreal morphStiffness = 260.0;
    qreal morphDamping = 0.95;

    // Behaviour.
    int peekTimeoutMs = 4500;     // default peek lifetime (-1 from plugins)
    int idleTimeoutMs = 0;        // collapse after this long without input (0 = off)
    QStringList disabledPlugins;
    QString terminal = QStringLiteral("kitty");

    // Embedded terminals (terminal_request).
    QString terminalFont = QStringLiteral("monospace");
    qreal terminalFontSize = 12.0;     // points
    int terminalScrollback = 2000;      // lines
    // "exclusive": a presented terminal takes the keyboard at once (released
    // when it retracts); "on_demand": the compositor focuses it on click.
    QString terminalKeyboard = QStringLiteral("exclusive");
    qreal terminalGap = 10.0;           // between the pill and the terminal
    qreal terminalPadding = 5.0;        // content inset
    bool debugCommands = false;   // enable render/pointer control commands
    QString monitors = QStringLiteral("all"); // "all" | "primary"
};

} // namespace shade::core
