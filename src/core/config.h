#pragma once

#include "settings.h"

#include <shade/theme.h>

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace shade::core
{

// The unified config file:
//   [shade]              core settings (Settings)
//   [theme]              color overrides ("bg" = "#000000", ...)
//   ["plugin:<name>"]    per-plugin sections, delivered to the plugin
struct Config
{
    QString path;
    Settings settings;
    Theme theme;
    QHash<QString, QVariantMap> plugins;
    QStringList warnings; // human readable, with file path

    QVariantMap pluginSection(const QString &name) const { return plugins.value(name); }

    // Missing/empty file -> defaults, no warnings. Bad syntax or wrong types
    // -> warnings, defaults for the affected values.
    static Config load(const QString &path);
    static Config parse(const QByteArray &toml, const QString &path = QStringLiteral("<string>"));
};

} // namespace shade::core
