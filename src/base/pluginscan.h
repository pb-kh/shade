#pragma once

#include <QString>
#include <QVector>

namespace shade::base
{

struct PluginInfo
{
    QString file;        // absolute path
    QString fileName;    // base name, used for ordering
    QString name;        // declared name
    QString author;
    QString description;
    QString version;
    QString api;         // "major.minor"
    bool valid = false;
    QString error;       // why it is not usable
};

// Reads the embedded metadata without loading the module.
PluginInfo inspectPlugin(const QString &file);
// All *.so in `dirs`, ordered by file name (alphabetically). When a declared
// name appears twice, the later directory wins (user overrides system); the
// replaced entry is dropped.
QVector<PluginInfo> scanPlugins(const QStringList &dirs);
bool apiCompatible(const QString &api, QString *why = nullptr);

} // namespace shade::base
