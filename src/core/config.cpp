#include "config.h"

#include <QColor>
#include <QFile>

#include <functional>
#include <sstream>

#define TOML_HEADER_ONLY 1
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

namespace shade::core
{

namespace
{

QVariant toVariant(const toml::node &node)
{
    if (auto v = node.as_string()) return QString::fromStdString(v->get());
    if (auto v = node.as_integer()) return QVariant::fromValue<qlonglong>(v->get());
    if (auto v = node.as_floating_point()) return v->get();
    if (auto v = node.as_boolean()) return v->get();
    if (auto arr = node.as_array()) {
        QVariantList list;
        for (const toml::node &e : *arr) list << toVariant(e);
        return list;
    }
    if (auto tbl = node.as_table()) {
        QVariantMap map;
        for (const auto &[k, v] : *tbl) map.insert(QString::fromStdString(std::string(k.str())), toVariant(v));
        return map;
    }
    if (auto v = node.as_date()) return QString::fromStdString((std::ostringstream() << v->get()).str());
    if (auto v = node.as_time()) return QString::fromStdString((std::ostringstream() << v->get()).str());
    if (auto v = node.as_date_time()) return QString::fromStdString((std::ostringstream() << v->get()).str());
    return {};
}

} // namespace

Config Config::load(const QString &path)
{
    QFile f(path);
    if (!f.exists()) {
        Config c;
        c.path = path;
        return c;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        Config c;
        c.path = path;
        c.warnings << QStringLiteral("%1: cannot read: %2").arg(path, f.errorString());
        return c;
    }
    return parse(f.readAll(), path);
}

Config Config::parse(const QByteArray &data, const QString &path)
{
    Config c;
    c.path = path;
    if (data.trimmed().isEmpty()) return c;

    toml::parse_result result = toml::parse(std::string_view(data.constData(), size_t(data.size())), path.toStdString());
    if (!result) {
        const auto &err = result.error();
        c.warnings << QStringLiteral("%1:%2:%3: %4 (using defaults)")
                          .arg(path)
                          .arg(err.source().begin.line)
                          .arg(err.source().begin.column)
                          .arg(QString::fromStdString(std::string(err.description())));
        return c;
    }
    const toml::table &root = result.table();

    auto warn = [&](const QString &key, const QString &what) {
        c.warnings << QStringLiteral("%1: [shade] %2: %3 (using default)").arg(path, key, what);
    };

    if (const toml::node *shadeNode = root.get("shade")) {
        const toml::table *shade = shadeNode->as_table();
        if (!shade) {
            c.warnings << QStringLiteral("%1: [shade] must be a table (using defaults)").arg(path);
        } else {
            Settings &s = c.settings;
            const std::pair<const char *, qreal *> reals[] = {
                {"bar_width", &s.barWidth},
                {"bar_height", &s.barHeight},
                {"top_offset", &s.topOffset},
                {"corner_radius", &s.cornerRadius},
                {"hover_zone", &s.hoverZone},
                {"reveal_zone", &s.revealZone},
                {"section_gap", &s.sectionGap},
                {"scroll_direction", &s.scrollDirection},
                {"touchpad_scroll_direction", &s.touchpadScrollDirection},
                {"sticky_distance", &s.stickyDistance},
                {"sticky_resistance", &s.stickyResistance},
                {"overscroll", &s.overscroll},
                {"overscroll_resistance", &s.overscrollResistance},
                {"advance_fraction", &s.advanceFraction},
                {"flick_speed", &s.flickSpeed},
                {"pixels_per_wheel_notch", &s.pixelsPerWheelNotch},
                {"peek_dismiss_distance", &s.peekDismissDistance},
                {"expand_stiffness", &s.expandStiffness},
                {"expand_damping", &s.expandDamping},
                {"collapse_stiffness", &s.collapseStiffness},
                {"collapse_damping", &s.collapseDamping},
                {"scroll_stiffness", &s.scrollStiffness},
                {"scroll_damping", &s.scrollDamping},
                {"morph_stiffness", &s.morphStiffness},
                {"morph_damping", &s.morphDamping},
                {"terminal_font_size", &s.terminalFontSize},
                {"terminal_gap", &s.terminalGap},
                {"terminal_padding", &s.terminalPadding},
            };
            const std::pair<const char *, int *> ints[] = {
                {"scroll_release_ms", &s.scrollReleaseMs},
                {"peek_timeout_ms", &s.peekTimeoutMs},
                {"idle_timeout_ms", &s.idleTimeoutMs},
                {"terminal_scrollback", &s.terminalScrollback},
            };
            QStringList known;
            for (const auto &[k, ptr] : reals) {
                known << QString::fromLatin1(k);
                const toml::node *n = shade->get(k);
                if (!n) continue;
                if (auto v = n->value<double>()) {
                    if (std::isfinite(*v)) *ptr = *v;
                    else warn(QString::fromLatin1(k), QStringLiteral("not a finite number"));
                } else {
                    warn(QString::fromLatin1(k), QStringLiteral("expected a number"));
                }
            }
            for (const auto &[k, ptr] : ints) {
                known << QString::fromLatin1(k);
                const toml::node *n = shade->get(k);
                if (!n) continue;
                if (auto v = n->as_integer()) *ptr = int(std::clamp<int64_t>(v->get(), -1, 86'400'000));
                else warn(QString::fromLatin1(k), QStringLiteral("expected an integer"));
            }
            const std::pair<const char *, QString *> strings[] = {
                {"terminal", &s.terminal},
                {"monitors", &s.monitors},
                {"terminal_font", &s.terminalFont},
                {"terminal_keyboard", &s.terminalKeyboard},
            };
            for (const auto &[k, ptr] : strings) {
                known << QString::fromLatin1(k);
                const toml::node *n = shade->get(k);
                if (!n) continue;
                if (auto v = n->as_string()) *ptr = QString::fromStdString(v->get());
                else warn(QString::fromLatin1(k), QStringLiteral("expected a string"));
            }
            if (s.terminalKeyboard != QLatin1String("on_demand") && s.terminalKeyboard != QLatin1String("exclusive")) {
                warn(QStringLiteral("terminal_keyboard"), QStringLiteral("expected \"on_demand\" or \"exclusive\""));
                s.terminalKeyboard = QStringLiteral("exclusive");
            }
            if (s.terminalFontSize < 4 || s.terminalFontSize > 72) {
                warn(QStringLiteral("terminal_font_size"), QStringLiteral("expected 4..72"));
                s.terminalFontSize = 12.0;
            }
            s.terminalGap = std::clamp<qreal>(s.terminalGap, 0, 200);
            s.terminalPadding = std::clamp<qreal>(s.terminalPadding, 0, 100);
            if (s.monitors != QLatin1String("all") && s.monitors != QLatin1String("primary")) {
                warn(QStringLiteral("monitors"), QStringLiteral("expected \"all\" or \"primary\""));
                s.monitors = QStringLiteral("all");
            }
            known << QStringLiteral("debug_commands") << QStringLiteral("disabled_plugins");
            if (const toml::node *n = shade->get("debug_commands")) {
                if (auto v = n->as_boolean()) s.debugCommands = v->get();
                else warn(QStringLiteral("debug_commands"), QStringLiteral("expected true/false"));
            }
            if (const toml::node *n = shade->get("disabled_plugins")) {
                if (auto arr = n->as_array()) {
                    for (const toml::node &e : *arr) {
                        if (auto v = e.as_string()) s.disabledPlugins << QString::fromStdString(v->get());
                        else warn(QStringLiteral("disabled_plugins"), QStringLiteral("entries must be strings"));
                    }
                } else {
                    warn(QStringLiteral("disabled_plugins"), QStringLiteral("expected an array of strings"));
                }
            }
            for (const auto &[k, v] : *shade) {
                const QString key = QString::fromStdString(std::string(k.str()));
                if (!known.contains(key)) c.warnings << QStringLiteral("%1: [shade] unknown key '%2' (ignored)").arg(path, key);
            }
            // Sanity clamps.
            s.barWidth = std::max<qreal>(1, s.barWidth);
            s.barHeight = std::max<qreal>(1, s.barHeight);
            s.scrollDirection = s.scrollDirection < 0 ? -1.0 : 1.0;
            s.advanceFraction = std::clamp<qreal>(s.advanceFraction, 0.01, 0.99);
            for (qreal *d : {&s.expandDamping, &s.collapseDamping, &s.scrollDamping, &s.morphDamping}) *d = std::clamp<qreal>(*d, 0.05, 4.0);
            for (qreal *k : {&s.expandStiffness, &s.collapseStiffness, &s.scrollStiffness, &s.morphStiffness}) *k = std::clamp<qreal>(*k, 1.0, 20000.0);
        }
    }

    if (const toml::node *themeNode = root.get("theme")) {
        if (const toml::table *theme = themeNode->as_table()) {
            for (const auto &[k, v] : *theme) {
                const QString key = QString::fromStdString(std::string(k.str()));
                const auto s = v.as_string();
                const QColor color = s ? QColor(QString::fromStdString(s->get())) : QColor();
                if (!color.isValid()) {
                    c.warnings << QStringLiteral("%1: [theme] %2: expected a color like \"#rrggbb\" (using default)").arg(path, key);
                    continue;
                }
                c.theme.setColor(key, color);
            }
        } else {
            c.warnings << QStringLiteral("%1: [theme] must be a table").arg(path);
        }
    }

    for (const auto &[k, v] : root) {
        const QString key = QString::fromStdString(std::string(k.str()));
        if (!key.startsWith(QLatin1String("plugin:"))) continue;
        const QString name = key.mid(7);
        if (const toml::table *t = v.as_table()) c.plugins.insert(name, toVariant(*t).toMap());
        else c.warnings << QStringLiteral("%1: [\"%2\"] must be a table").arg(path, key);
    }
    return c;
}

} // namespace shade::core
