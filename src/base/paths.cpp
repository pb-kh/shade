#include "paths.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <sys/stat.h>
#include <unistd.h>

#ifndef SHADE_SYSTEM_PLUGIN_DIR
#define SHADE_SYSTEM_PLUGIN_DIR "/usr/lib/shade/plugins"
#endif

namespace shade::base
{

namespace
{
QString env(const char *name) { return QString::fromLocal8Bit(qgetenv(name)); }

QStringList splitPath(const QString &s)
{
    QStringList out;
    for (const QString &p : s.split(u':', Qt::SkipEmptyParts)) out << QDir::cleanPath(p);
    return out;
}
} // namespace

QString runtimeDir()
{
    QString dir = env("SHADE_RUNTIME_DIR");
    if (dir.isEmpty()) {
        const QString xdg = env("XDG_RUNTIME_DIR");
        dir = xdg.isEmpty() ? QStringLiteral("/tmp/shade-%1").arg(getuid()) : xdg + QStringLiteral("/shade");
    }
    QDir().mkpath(dir);
    ::chmod(QFile::encodeName(dir).constData(), 0700);
    return dir;
}

QString pidPath() { return runtimeDir() + QStringLiteral("/shade.pid"); }
QString socketPath() { return runtimeDir() + QStringLiteral("/shade.sock"); }
QString logPath() { return runtimeDir() + QStringLiteral("/shade.log"); }

QString configPath()
{
    QString base = env("XDG_CONFIG_HOME");
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.config");
    return base + QStringLiteral("/shade/config.toml");
}

QString userPluginDir()
{
    QString base = env("XDG_STATE_HOME");
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/shade/plugins");
}

QString systemPluginDir() { return QStringLiteral(SHADE_SYSTEM_PLUGIN_DIR); }

QStringList pluginSearchDirs()
{
    const QString override = env("SHADE_PLUGIN_PATH");
    if (!override.isEmpty()) return splitPath(override);
    return {systemPluginDir(), userPluginDir()};
}

QStringList availablePluginDirs()
{
    const QString override = env("SHADE_AVAILABLE_PLUGIN_PATH");
    if (!override.isEmpty()) return splitPath(override);
    return {systemPluginDir()};
}

} // namespace shade::base
