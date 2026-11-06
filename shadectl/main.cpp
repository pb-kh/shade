// shadectl — control a running shade and manage plugins.
//
// QtCore/QtNetwork only (no GUI code): it talks to the shade over its
// control socket and reads plugin metadata without loading plugins.

#include "base/paths.h"
#include "base/pidlock.h"
#include "base/pluginscan.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>

#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace shade;

namespace
{

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

int fail(const QString &message, int code = 1)
{
    out().flush();
    std::fprintf(stderr, "shadectl: %s\n", qPrintable(message));
    return code;
}

struct Command
{
    QStringList names; // canonical first
    QString syntax;
    QString summary;
};

const QList<Command> kTop = {
    {{QStringLiteral("start")}, QStringLiteral("[--config <path>]"), QStringLiteral("Start shade in the background")},
    {{QStringLiteral("stop")}, {}, QStringLiteral("Stop the running shade")},
    {{QStringLiteral("restart")}, QStringLiteral("[--config <path>]"), QStringLiteral("Restart shade")},
    {{QStringLiteral("status")}, {}, QStringLiteral("Show whether shade is running")},
    {{QStringLiteral("state")}, QStringLiteral("[--json]"), QStringLiteral("Show windows, panes and queued peeks")},
    {{QStringLiteral("expand")}, QStringLiteral("[--window <name>]"), QStringLiteral("Expand the shade")},
    {{QStringLiteral("collapse")}, QStringLiteral("[--window <name>]"), QStringLiteral("Collapse the shade")},
    {{QStringLiteral("focus")}, QStringLiteral("<plugin> [--window <name>]"), QStringLiteral("Expand and show a plugin's widget")},
    {{QStringLiteral("dismiss")}, QStringLiteral("[<plugin> [<peek id>]]"), QStringLiteral("Dismiss peeks (all when no plugin given)")},
    {{QStringLiteral("signal"), QStringLiteral("emit")}, QStringLiteral("<destination> <name> [<json payload>]"), QStringLiteral("Send a signal on the shade bus")},
    {{QStringLiteral("ping")}, {}, QStringLiteral("Check the control socket")},
    {{QStringLiteral("plugins"), QStringLiteral("pl"), QStringLiteral("plug")}, QStringLiteral("<action>"), QStringLiteral("Manage plugins: list, install, remove")},
    {{QStringLiteral("help"), QStringLiteral("--help"), QStringLiteral("-h")}, {}, QStringLiteral("Show this help")},
};

const QList<Command> kPluginActions = {
    {{QStringLiteral("list"), QStringLiteral("ls")}, {}, QStringLiteral("List available plugins and whether they are installed")},
    {{QStringLiteral("install"), QStringLiteral("i"), QStringLiteral("in"), QStringLiteral("add")}, QStringLiteral("<plugin|file.so>... [--as <file-base>]"), QStringLiteral("Install plugins into the user plugin directory")},
    {{QStringLiteral("remove"), QStringLiteral("rm"), QStringLiteral("uninstall"), QStringLiteral("un")}, QStringLiteral("<plugin>..."), QStringLiteral("Remove installed plugins")},
    {{QStringLiteral("help"), QStringLiteral("--help"), QStringLiteral("-h")}, {}, QStringLiteral("Show this help")},
};

const Command *find(const QList<Command> &table, const QString &name)
{
    for (const Command &c : table) {
        if (c.names.contains(name)) return &c;
    }
    return nullptr;
}

void printTable(const QList<Command> &table, const QString &prefix)
{
    int width = 0;
    QStringList left;
    for (const Command &c : table) {
        const QString l = c.names.join(QStringLiteral(", ")) + (c.syntax.isEmpty() ? QString() : u' ' + c.syntax);
        width = std::max(width, int(l.size()));
        left << l;
    }
    for (int i = 0; i < table.size(); ++i) out() << "  " << prefix << left[i].leftJustified(width + 2) << table[i].summary << '\n';
}

void usage()
{
    out() << "Usage: shadectl <command> [args]\n\nCommands:\n";
    printTable(kTop, {});
    out() << "\nRun 'shadectl plugins --help' for the plugin commands.\n"
          << "Debug commands (need debug_commands = true): pointer <x> <y>, leave, click <x> <y>,\n"
          << "  wheel <dx> <dy> [--touchpad] [--end], type <text> [--owner <plugin>],\n  render <file.png>, send '<json request>'.\n";
}

void pluginUsage()
{
    out() << "Usage: shadectl plugins <action> [args]\n\nActions:\n";
    printTable(kPluginActions, {});
    out() << "\nPlugins are installed into " << base::userPluginDir() << "\n"
          << "and installed from " << base::availablePluginDirs().join(QStringLiteral(", ")) << ".\n";
}

// --- control socket -------------------------------------------------------

bool request(const QJsonObject &req, QJsonObject *reply, QString *error, int timeoutMs = 3000)
{
    QLocalSocket sock;
    sock.connectToServer(base::socketPath());
    if (!sock.waitForConnected(timeoutMs)) {
        *error = QStringLiteral("shade is not running (%1)").arg(base::socketPath());
        return false;
    }
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    sock.flush();
    QElapsedTimer t;
    t.start();
    QByteArray buf;
    while (!buf.contains('\n')) {
        if (!sock.waitForReadyRead(std::max<qint64>(1, timeoutMs - t.elapsed()))) {
            *error = QStringLiteral("no reply from shade");
            return false;
        }
        buf += sock.readAll();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(buf.left(buf.indexOf('\n')));
    if (!doc.isObject()) {
        *error = QStringLiteral("malformed reply from shade");
        return false;
    }
    *reply = doc.object();
    return true;
}

int call(const QJsonObject &req, QJsonObject *replyOut = nullptr)
{
    QJsonObject reply;
    QString error;
    if (!request(req, &reply, &error)) return fail(error);
    if (!reply.value(QStringLiteral("ok")).toBool()) return fail(reply.value(QStringLiteral("error")).toString());
    if (replyOut) *replyOut = reply;
    return 0;
}

// Splits "--window X" out of args.
QString takeOption(QStringList &args, const QString &name)
{
    const int i = args.indexOf(name);
    if (i < 0) return {};
    const QString v = args.value(i + 1);
    args.remove(i, std::min<qsizetype>(2, args.size() - i));
    return v;
}

bool takeFlag(QStringList &args, const QString &name)
{
    const int i = args.indexOf(name);
    if (i < 0) return false;
    args.removeAt(i);
    return true;
}

// --- process control --------------------------------------------------------

QString shadeBinary()
{
    const QString env = qEnvironmentVariable("SHADE_BIN");
    if (!env.isEmpty()) return env;
    const QString sibling = QCoreApplication::applicationDirPath() + QStringLiteral("/shade");
    if (QFileInfo(sibling).isExecutable()) return sibling;
    const QString sub = QCoreApplication::applicationDirPath() + QStringLiteral("/../src/app/shade");
    if (QFileInfo(sub).isExecutable()) return QFileInfo(sub).canonicalFilePath();
    return QStandardPaths::findExecutable(QStringLiteral("shade"));
}

pid_t runningPid()
{
    const pid_t pid = base::readPid(base::pidPath());
    return base::pidAlive(pid) ? pid : 0;
}

int cmdStart(QStringList args)
{
    const QString config = takeOption(args, QStringLiteral("--config"));
    if (!args.isEmpty()) return fail(QStringLiteral("start: unexpected argument '%1'").arg(args.first()), 2);
    if (const pid_t pid = runningPid()) {
        out() << "shade is already running (PID " << pid << ")\n";
        return 0;
    }
    const QString bin = shadeBinary();
    if (bin.isEmpty()) return fail(QStringLiteral("cannot find the shade executable (set SHADE_BIN)"));
    QStringList argv{bin};
    if (!config.isEmpty()) argv << QStringLiteral("--config") << config;

    const pid_t child = fork();
    if (child < 0) return fail(QStringLiteral("cannot fork"));
    if (child == 0) {
        // Detach: new session, survives the terminal; output to the log.
        setsid();
        if (fork() > 0) _exit(0);
        const QByteArray log = QFile::encodeName(base::logPath());
        const int fd = ::open(log.constData(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        std::vector<QByteArray> bytes;
        for (const QString &a : argv) bytes.push_back(QFile::encodeName(a));
        std::vector<char *> cargv;
        for (QByteArray &b : bytes) cargv.push_back(b.data());
        cargv.push_back(nullptr);
        execv(cargv[0], cargv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    // Wait until it answers on the socket.
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 5000) {
        QJsonObject reply;
        QString error;
        if (request({{QStringLiteral("cmd"), QStringLiteral("ping")}}, &reply, &error, 200)) {
            out() << "shade started (PID " << reply.value(QStringLiteral("pid")).toInteger() << ")\n";
            return 0;
        }
        QThread::msleep(50);
    }
    return fail(QStringLiteral("shade did not come up; see %1").arg(base::logPath()));
}

int cmdStop(const QStringList &args)
{
    if (!args.isEmpty()) return fail(QStringLiteral("stop: unexpected argument '%1'").arg(args.first()), 2);
    const pid_t pid = runningPid();
    if (!pid) {
        out() << "shade is not running\n";
        return 0;
    }
    if (::kill(pid, SIGTERM) != 0) return fail(QStringLiteral("cannot signal PID %1").arg(pid));
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 5000) {
        if (!base::pidAlive(pid)) {
            out() << "shade stopped\n";
            return 0;
        }
        QThread::msleep(25);
    }
    return fail(QStringLiteral("shade (PID %1) did not stop within 5 s").arg(pid));
}

int cmdStatus()
{
    const pid_t pid = runningPid();
    if (!pid) {
        out() << "shade is not running\n";
        return 3;
    }
    QJsonObject reply;
    QString error;
    const bool sock = request({{QStringLiteral("cmd"), QStringLiteral("version")}}, &reply, &error, 1000);
    out() << "shade is running (PID " << pid << ")";
    if (sock) out() << ", version " << reply.value(QStringLiteral("version")).toString() << ", plugin API " << reply.value(QStringLiteral("api")).toString();
    else out() << ", control socket unavailable";
    out() << '\n';
    return 0;
}

int cmdState(QStringList args)
{
    const bool json = takeFlag(args, QStringLiteral("--json"));
    QJsonObject reply;
    if (int rc = call({{QStringLiteral("cmd"), QStringLiteral("state")}}, &reply)) return rc;
    reply.remove(QStringLiteral("ok"));
    if (json) {
        out() << QJsonDocument(reply).toJson(QJsonDocument::Indented);
        return 0;
    }
    for (const QJsonValue &wv : reply.value(QStringLiteral("windows")).toArray()) {
        const QJsonObject w = wv.toObject();
        out() << w.value(QStringLiteral("name")).toString() << ": " << w.value(QStringLiteral("state")).toString();
        if (!w.value(QStringLiteral("selected")).toString().isEmpty()) out() << ", pane " << w.value(QStringLiteral("selected")).toString();
        if (w.value(QStringLiteral("live")).toBool()) out() << " (live)";
        out() << '\n';
        QStringList panes;
        for (const QJsonValue &p : w.value(QStringLiteral("panes")).toArray()) panes << p.toObject().value(QStringLiteral("plugin")).toString();
        out() << "  panes: " << panes.join(QStringLiteral(", ")) << '\n';
        const QJsonArray peeks = w.value(QStringLiteral("peeks")).toArray();
        out() << "  peeks: " << (peeks.isEmpty() ? QStringLiteral("none") : QString()) << '\n';
        for (const QJsonValue &pv : peeks) {
            const QJsonObject p = pv.toObject();
            out() << "    " << p.value(QStringLiteral("plugin")).toString() << '/' << p.value(QStringLiteral("id")).toString()
                  << " priority " << p.value(QStringLiteral("priority")).toInt() << ", expand " << p.value(QStringLiteral("expand")).toString() << '\n';
        }
        for (const QJsonValue &tv : w.value(QStringLiteral("terminals")).toArray()) {
            const QJsonObject t = tv.toObject();
            const QJsonObject grid = t.value(QStringLiteral("grid")).toObject();
            out() << "  terminal: " << t.value(QStringLiteral("owner")).toString() << ' ' << t.value(QStringLiteral("state")).toString()
                  << ", " << grid.value(QStringLiteral("cols")).toInt() << 'x' << grid.value(QStringLiteral("rows")).toInt()
                  << ", pid " << t.value(QStringLiteral("pid")).toInteger() << (t.value(QStringLiteral("running")).toBool() ? "" : " (exited)") << '\n';
        }
    }
    return 0;
}

// --- plugins ----------------------------------------------------------------

QVector<base::PluginInfo> available()
{
    QVector<base::PluginInfo> out;
    for (const QString &dir : base::availablePluginDirs()) {
        for (const QFileInfo &fi : QDir(dir).entryInfoList({QStringLiteral("*.so")}, QDir::Files, QDir::Name)) {
            base::PluginInfo info = base::inspectPlugin(fi.absoluteFilePath());
            if (!info.name.isEmpty()) out << info;
        }
    }
    return out;
}

QVector<base::PluginInfo> installed()
{
    QVector<base::PluginInfo> out;
    for (const QFileInfo &fi : QDir(base::userPluginDir()).entryInfoList({QStringLiteral("*.so")}, QDir::Files, QDir::Name)) {
        out << base::inspectPlugin(fi.absoluteFilePath());
    }
    return out;
}

int pluginsList()
{
    const QVector<base::PluginInfo> avail = available();
    const QVector<base::PluginInfo> inst = installed();
    QStringList instNames;
    for (const auto &p : inst) instNames << p.name;
    QList<QStringList> rows{{QStringLiteral("NAME"), QStringLiteral("AUTHOR"), QStringLiteral("VERSION"), QStringLiteral("API"), QStringLiteral("INSTALLED"), QStringLiteral("DESCRIPTION")}};
    QStringList seen;
    auto add = [&](const base::PluginInfo &p) {
        if (seen.contains(p.name)) return;
        seen << p.name;
        rows << QStringList{p.name, p.author, p.version, p.valid ? p.api : p.api + QStringLiteral(" (incompatible)"),
                            instNames.contains(p.name) ? QStringLiteral("yes") : QStringLiteral("no"), p.description};
    };
    for (const auto &p : avail) add(p);
    for (const auto &p : inst) add(p);
    if (rows.size() == 1) {
        out() << "no plugins available in " << base::availablePluginDirs().join(QStringLiteral(", ")) << '\n';
        return 0;
    }
    QVector<int> widths(rows[0].size(), 0);
    for (const QStringList &r : rows)
        for (int i = 0; i < r.size(); ++i) widths[i] = std::max(widths[i], int(r[i].size()));
    for (const QStringList &r : rows) {
        for (int i = 0; i < r.size(); ++i) out() << (i + 1 < r.size() ? r[i].leftJustified(widths[i] + 2) : r[i]);
        out() << '\n';
    }
    return 0;
}

int pluginsInstall(QStringList args)
{
    QStringList as;
    for (int i = args.indexOf(QStringLiteral("--as")); i >= 0; i = args.indexOf(QStringLiteral("--as"))) {
        if (i + 1 >= args.size()) return fail(QStringLiteral("install: --as needs a file base name"), 2);
        as << args[i + 1];
        args.remove(i, 2);
    }
    if (args.isEmpty()) return fail(QStringLiteral("install: name at least one plugin"), 2);
    if (!as.isEmpty() && as.size() != args.size()) return fail(QStringLiteral("install: give one --as per plugin"), 2);
    QDir().mkpath(base::userPluginDir());
    const QVector<base::PluginInfo> avail = available();
    int rc = 0;
    for (int i = 0; i < args.size(); ++i) {
        const QString what = args[i];
        base::PluginInfo src;
        if (what.endsWith(QLatin1String(".so")) || what.contains(u'/')) {
            src = base::inspectPlugin(what);
        } else {
            for (const auto &p : avail) {
                if (p.name == what) src = p;
            }
        }
        if (src.name.isEmpty()) {
            rc = fail(QStringLiteral("install: no plugin named '%1' in %2").arg(what, base::availablePluginDirs().join(QStringLiteral(", "))));
            continue;
        }
        if (!src.valid) {
            rc = fail(QStringLiteral("install: %1: %2").arg(what, src.error));
            continue;
        }
        QString base = as.isEmpty() ? QFileInfo(src.file).completeBaseName() : as[i];
        if (base.contains(u'/') || base.isEmpty()) {
            rc = fail(QStringLiteral("install: invalid file base '%1'").arg(base), 2);
            continue;
        }
        const QString target = base::userPluginDir() + u'/' + base + QStringLiteral(".so");
        // One declared name, one file: drop older copies of this plugin, and
        // refuse to overwrite a different plugin's file.
        bool clash = false;
        for (const auto &p : installed()) {
            if (p.file == target && p.name != src.name && !p.name.isEmpty()) clash = true;
        }
        if (clash) {
            rc = fail(QStringLiteral("install: %1 belongs to another plugin").arg(target));
            continue;
        }
        for (const auto &p : installed()) {
            if (p.name == src.name && p.file != target) QFile::remove(p.file);
        }
        const QString tmp = target + QStringLiteral(".tmp");
        QFile::remove(tmp);
        if (!QFile::copy(src.file, tmp) || (QFile::exists(target) && !QFile::remove(target)) || !QFile::rename(tmp, target)) {
            QFile::remove(tmp);
            rc = fail(QStringLiteral("install: cannot write %1").arg(target));
            continue;
        }
        out() << "installed " << src.name << ' ' << src.version << " -> " << target << '\n';
    }
    if (rc == 0 && runningPid()) out() << "restart shade to apply (shadectl restart)\n";
    return rc;
}

int pluginsRemove(const QStringList &args)
{
    if (args.isEmpty()) return fail(QStringLiteral("remove: name at least one plugin"), 2);
    int rc = 0;
    for (const QString &name : args) {
        bool any = false;
        for (const auto &p : installed()) {
            if (p.name == name) {
                if (!QFile::remove(p.file)) rc = fail(QStringLiteral("remove: cannot delete %1").arg(p.file));
                else out() << "removed " << name << " (" << p.file << ")\n";
                any = true;
            }
        }
        if (!any) rc = fail(QStringLiteral("remove: '%1' is not installed").arg(name));
    }
    return rc;
}

int cmdPlugins(QStringList args)
{
    if (args.isEmpty()) {
        pluginUsage();
        return 2;
    }
    const QString action = args.takeFirst();
    const Command *c = find(kPluginActions, action);
    if (!c) return fail(QStringLiteral("plugins: unknown action '%1' (see 'shadectl plugins --help')").arg(action), 2);
    const QString canon = c->names.first();
    if (canon == QLatin1String("help")) {
        pluginUsage();
        return 0;
    }
    if (canon == QLatin1String("list")) {
        if (!args.isEmpty()) return fail(QStringLiteral("plugins list: unexpected argument '%1'").arg(args.first()), 2);
        return pluginsList();
    }
    if (canon == QLatin1String("install")) return pluginsInstall(args);
    return pluginsRemove(args);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("shadectl"));
    QStringList args = app.arguments().mid(1);
    if (args.isEmpty()) {
        usage();
        return 0;
    }
    const QString cmd = args.takeFirst();
    const QString window = takeOption(args, QStringLiteral("--window"));
    auto withWindow = [&](QJsonObject o) {
        if (!window.isEmpty()) o.insert(QStringLiteral("window"), window);
        return o;
    };

    if (const Command *c = find(kTop, cmd)) {
        const QString canon = c->names.first();
        if (canon == QLatin1String("help")) {
            usage();
            return 0;
        }
        if (canon == QLatin1String("start")) return cmdStart(args);
        if (canon == QLatin1String("stop")) return cmdStop(args);
        if (canon == QLatin1String("restart")) {
            const QString config = takeOption(args, QStringLiteral("--config"));
            if (int rc = cmdStop({})) return rc;
            return cmdStart(config.isEmpty() ? QStringList{} : QStringList{QStringLiteral("--config"), config});
        }
        if (canon == QLatin1String("status")) return cmdStatus();
        if (canon == QLatin1String("state")) return cmdState(args);
        if (canon == QLatin1String("plugins")) return cmdPlugins(args);
        if (canon == QLatin1String("ping")) {
            if (int rc = call({{QStringLiteral("cmd"), QStringLiteral("ping")}})) return rc;
            out() << "pong\n";
            return 0;
        }
        if (canon == QLatin1String("expand") || canon == QLatin1String("collapse")) return call(withWindow({{QStringLiteral("cmd"), canon}}));
        if (canon == QLatin1String("focus")) {
            if (args.size() != 1) return fail(QStringLiteral("usage: shadectl focus <plugin>"), 2);
            return call(withWindow({{QStringLiteral("cmd"), canon}, {QStringLiteral("plugin"), args[0]}}));
        }
        if (canon == QLatin1String("dismiss")) {
            return call(withWindow({{QStringLiteral("cmd"), canon}, {QStringLiteral("plugin"), args.value(0)}, {QStringLiteral("id"), args.value(1)}}));
        }
        if (canon == QLatin1String("signal")) {
            if (args.size() < 2 || args.size() > 3) return fail(QStringLiteral("usage: shadectl signal <destination> <name> [<json payload>]"), 2);
            QJsonObject payload;
            if (args.size() == 3) {
                const QJsonDocument doc = QJsonDocument::fromJson(args[2].toUtf8());
                if (!doc.isObject()) return fail(QStringLiteral("signal: payload must be a JSON object"), 2);
                payload = doc.object();
            }
            return call(withWindow({{QStringLiteral("cmd"), QStringLiteral("signal")}, {QStringLiteral("destination"), args[0]},
                                    {QStringLiteral("name"), args[1]}, {QStringLiteral("payload"), payload}}));
        }
    }

    // Debug commands.
    if (cmd == QLatin1String("pointer") || cmd == QLatin1String("click")) {
        if (args.size() != 2) return fail(QStringLiteral("usage: shadectl %1 <x> <y>").arg(cmd), 2);
        return call(withWindow({{QStringLiteral("cmd"), cmd}, {QStringLiteral("x"), args[0].toDouble()}, {QStringLiteral("y"), args[1].toDouble()}}));
    }
    if (cmd == QLatin1String("leave")) return call(withWindow({{QStringLiteral("cmd"), cmd}}));
    if (cmd == QLatin1String("wheel")) {
        const bool end = takeFlag(args, QStringLiteral("--end"));
        const bool touchpad = takeFlag(args, QStringLiteral("--touchpad"));
        if (!end && args.size() != 2) return fail(QStringLiteral("usage: shadectl wheel <dx> <dy> [--touchpad] | --end"), 2);
        return call(withWindow({{QStringLiteral("cmd"), cmd}, {QStringLiteral("dx"), args.value(0).toDouble()},
                                {QStringLiteral("dy"), args.value(1).toDouble()}, {QStringLiteral("end"), end},
                                {QStringLiteral("touchpad"), touchpad}}));
    }
    if (cmd == QLatin1String("type")) {
        // type <text> [--owner <plugin>]: send text (\n = Enter) to a terminal.
        const QString owner = takeOption(args, QStringLiteral("--owner"));
        if (args.size() != 1) return fail(QStringLiteral("usage: shadectl type <text> [--owner <plugin>]"), 2);
        QString text = args[0];
        text.replace(QLatin1String("\\n"), QLatin1String("\n"));
        return call(withWindow({{QStringLiteral("cmd"), cmd}, {QStringLiteral("text"), text}, {QStringLiteral("owner"), owner}}));
    }
    if (cmd == QLatin1String("render")) {
        if (args.size() != 1) return fail(QStringLiteral("usage: shadectl render <file.png>"), 2);
        return call(withWindow({{QStringLiteral("cmd"), cmd}, {QStringLiteral("path"), QFileInfo(args[0]).absoluteFilePath()}}));
    }
    if (cmd == QLatin1String("send")) {
        const QJsonDocument doc = QJsonDocument::fromJson(args.value(0).toUtf8());
        if (!doc.isObject()) return fail(QStringLiteral("send: expected a JSON object"), 2);
        QJsonObject reply;
        if (int rc = call(doc.object(), &reply)) return rc;
        out() << QJsonDocument(reply).toJson(QJsonDocument::Compact) << '\n';
        return 0;
    }
    std::fprintf(stderr, "shadectl: unknown command '%s'\n\n", qPrintable(cmd));
    usage();
    return 2;
}
