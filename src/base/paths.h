#pragma once

#include <QString>
#include <QStringList>

namespace shade::base
{

// $SHADE_RUNTIME_DIR or $XDG_RUNTIME_DIR/shade (0700). Created on demand.
QString runtimeDir();
QString pidPath();
QString socketPath();
QString logPath();

// $XDG_CONFIG_HOME/shade/config.toml (fallback ~/.config/shade/config.toml).
QString configPath();

// ~/.local/state/shade/plugins (XDG_STATE_HOME).
QString userPluginDir();
// Where bundled plugins are installed (compile time).
QString systemPluginDir();
// Directories the host loads plugins from: $SHADE_PLUGIN_PATH (colon
// separated) when set, otherwise system then user.
QStringList pluginSearchDirs();
// Directories `shadectl plugins install` copies from:
// $SHADE_AVAILABLE_PLUGIN_PATH when set, otherwise the system dir.
QStringList availablePluginDirs();

} // namespace shade::base
