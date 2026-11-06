#include "base/paths.h"
#include "base/pidlock.h"
#include "core/config.h"
#include "host.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QSocketNotifier>

#include <csignal>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
int g_sigpipe[2] = {-1, -1};

void onTermSignal(int sig)
{
    const char c = char(sig);
    [[maybe_unused]] ssize_t n = ::write(g_sigpipe[1], &c, 1);
}

void onFatalSignal(int sig)
{
    shade::base::runPidCleanup();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const char *level = type == QtDebugMsg ? "debug" : type == QtInfoMsg ? "info" : type == QtWarningMsg ? "warn" : "error";
    const QByteArray line = QStringLiteral("%1 [%2] %3\n")
                                .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                                     QString::fromLatin1(level), msg)
                                .toLocal8Bit();
    std::fwrite(line.constData(), 1, size_t(line.size()), stderr);
    std::fflush(stderr);
    if (type == QtFatalMsg) std::abort();
}
} // namespace

int main(int argc, char *argv[])
{
    qInstallMessageHandler(messageHandler);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("shade"));
    QApplication::setApplicationVersion(QStringLiteral(SHADE_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("shade"));
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A dynamic overlay bar for Wayland."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption configOpt(QStringLiteral("config"), QStringLiteral("Config file (default: %1)").arg(shade::base::configPath()),
                                 QStringLiteral("path"));
    parser.addOption(configOpt);
    parser.process(app);

    shade::base::PidLock lock(shade::base::pidPath());
    switch (lock.acquire()) {
    case shade::base::PidLock::Acquired:
        break;
    case shade::base::PidLock::AlreadyRunning:
        std::fprintf(stderr, "shade is already running (PID %d)\n", int(lock.runningPid()));
        return 1;
    case shade::base::PidLock::Error:
        std::fprintf(stderr, "shade: %s\n", qPrintable(lock.error()));
        return 1;
    }
    shade::base::installPidCleanup(lock.path(), shade::base::socketPath());

    // SIGTERM/INT/HUP: quit cleanly from the event loop.
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, g_sigpipe) == 0) {
        auto *notifier = new QSocketNotifier(g_sigpipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [] {
            char c;
            [[maybe_unused]] ssize_t n = ::read(g_sigpipe[0], &c, 1);
            qInfo("shade: signal %d, quitting", int(c));
            QCoreApplication::quit();
        });
        for (int s : {SIGTERM, SIGINT, SIGHUP}) std::signal(s, onTermSignal);
    }
    for (int s : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) std::signal(s, onFatalSignal);
    std::signal(SIGPIPE, SIG_IGN);

    const QString configPath = parser.isSet(configOpt) ? parser.value(configOpt) : shade::base::configPath();
    shade::app::ShadeHost host(shade::core::Config::load(configPath));
    QString error;
    if (!host.start(&error)) {
        std::fprintf(stderr, "shade: %s\n", qPrintable(error));
        return 1;
    }
    const int rc = app.exec();
    host.shutdown();
    lock.release();
    return rc;
}
