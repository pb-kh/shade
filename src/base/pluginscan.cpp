#include "pluginscan.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QPluginLoader>
#include <QRegularExpression>

#include <algorithm>

#define SHADE_IID "org.shade.Plugin/1"
#ifndef SHADE_API_MAJOR
#define SHADE_API_MAJOR 1
#define SHADE_API_MINOR 1
#endif

namespace shade::base
{

bool apiCompatible(const QString &api, QString *why)
{
    const QStringList parts = api.split(u'.');
    bool okMajor = false, okMinor = true;
    const int major = parts.value(0).toInt(&okMajor);
    const int minor = parts.size() > 1 ? parts.value(1).toInt(&okMinor) : 0;
    if (!okMajor || !okMinor) {
        if (why) *why = QStringLiteral("invalid api version '%1'").arg(api);
        return false;
    }
    if (major != SHADE_API_MAJOR || minor > SHADE_API_MINOR) {
        if (why) *why = QStringLiteral("needs api %1, host provides %2.%3").arg(api).arg(SHADE_API_MAJOR).arg(SHADE_API_MINOR);
        return false;
    }
    return true;
}

PluginInfo inspectPlugin(const QString &file)
{
    PluginInfo info;
    const QFileInfo fi(file);
    info.file = fi.absoluteFilePath();
    info.fileName = fi.fileName();
    QPluginLoader loader(info.file);
    const QJsonObject meta = loader.metaData();
    if (meta.isEmpty()) {
        info.error = QStringLiteral("not a shade plugin (no metadata)");
        return info;
    }
    if (meta.value(QStringLiteral("IID")).toString() != QLatin1String(SHADE_IID)) {
        info.error = QStringLiteral("not a shade plugin (IID %1)").arg(meta.value(QStringLiteral("IID")).toString());
        return info;
    }
    const QJsonObject md = meta.value(QStringLiteral("MetaData")).toObject();
    info.name = md.value(QStringLiteral("name")).toString();
    info.author = md.value(QStringLiteral("author")).toString();
    info.description = md.value(QStringLiteral("description")).toString();
    info.version = md.value(QStringLiteral("version")).toString();
    info.api = md.value(QStringLiteral("api")).toString();
    static const QRegularExpression nameRe(QStringLiteral("^[A-Za-z][A-Za-z0-9_-]{0,63}$"));
    if (!nameRe.match(info.name).hasMatch()) {
        info.error = QStringLiteral("invalid plugin name '%1'").arg(info.name);
        return info;
    }
    QString why;
    if (!apiCompatible(info.api, &why)) {
        info.error = why;
        return info;
    }
    info.valid = true;
    return info;
}

QVector<PluginInfo> scanPlugins(const QStringList &dirs)
{
    QVector<PluginInfo> out;
    for (const QString &dir : dirs) {
        const QDir d(dir);
        if (!d.exists()) continue;
        for (const QFileInfo &fi : d.entryInfoList({QStringLiteral("*.so")}, QDir::Files | QDir::Readable, QDir::Name)) {
            PluginInfo info = inspectPlugin(fi.absoluteFilePath());
            if (info.valid) {
                // Later directories override earlier ones by declared name.
                out.removeIf([&](const PluginInfo &o) { return o.valid && o.name == info.name; });
            }
            out.push_back(info);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const PluginInfo &a, const PluginInfo &b) {
        return a.fileName < b.fileName;
    });
    return out;
}

} // namespace shade::base
