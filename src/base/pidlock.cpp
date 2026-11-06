#include "pidlock.h"

#include <QFile>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shade::base
{

pid_t parsePid(const QByteArray &content)
{
    const QByteArray s = content.trimmed();
    if (s.isEmpty() || s.size() > 10) return 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
    }
    bool ok = false;
    const long v = s.toLong(&ok);
    // 0 and 1 (init) and negative values reach kill() with special meaning.
    if (!ok || v < 2 || v > INT_MAX) return 0;
    return static_cast<pid_t>(v);
}

pid_t readPid(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    return parsePid(f.read(64));
}

bool pidAlive(pid_t pid)
{
    if (pid < 2) return false;
    if (::kill(pid, 0) != 0 && errno != EPERM) return false;
    // A zombie answers signal 0; treat it as dead.
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (stat.open(QIODevice::ReadOnly)) {
        const QByteArray s = stat.readAll();
        const int close = s.lastIndexOf(')');
        if (close > 0 && close + 2 < s.size() && s.at(close + 2) == 'Z') return false;
    }
    return true;
}

PidLock::PidLock(QString path)
    : m_path(std::move(path))
{
}

PidLock::~PidLock() { release(); }

PidLock::Result PidLock::acquire()
{
    const QByteArray p = QFile::encodeName(m_path);
    for (int attempt = 0; attempt < 3; ++attempt) {
        struct stat st;
        if (::lstat(p.constData(), &st) == 0) {
            if (!S_ISREG(st.st_mode)) {
                m_error = QStringLiteral("%1 exists and is not a regular file").arg(m_path);
                return Error;
            }
            const pid_t other = readPid(m_path);
            if (other > 0 && other != getpid() && pidAlive(other)) {
                m_running = other;
                return AlreadyRunning;
            }
            ::unlink(p.constData()); // stale
        }
        const int fd = ::open(p.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue; // raced: re-check
            m_error = QStringLiteral("cannot create %1: %2").arg(m_path, QString::fromLocal8Bit(strerror(errno)));
            return Error;
        }
        const QByteArray content = QByteArray::number(getpid()) + '\n';
        if (::write(fd, content.constData(), content.size()) != content.size()) {
            ::close(fd);
            ::unlink(p.constData());
            m_error = QStringLiteral("cannot write %1").arg(m_path);
            return Error;
        }
        m_fd = fd;
        return Acquired;
    }
    m_error = QStringLiteral("cannot acquire %1").arg(m_path);
    return Error;
}

void PidLock::release()
{
    if (m_fd < 0) return;
    if (readPid(m_path) == getpid()) ::unlink(QFile::encodeName(m_path).constData());
    ::close(m_fd);
    m_fd = -1;
}

namespace
{
char g_pidPath[PATH_MAX];
char g_sockPath[PATH_MAX];
} // namespace

void installPidCleanup(const QString &path, const QString &socket)
{
    qstrncpy(g_pidPath, QFile::encodeName(path).constData(), sizeof g_pidPath);
    qstrncpy(g_sockPath, QFile::encodeName(socket).constData(), sizeof g_sockPath);
}

void runPidCleanup()
{
    if (g_pidPath[0]) ::unlink(g_pidPath);
    if (g_sockPath[0]) ::unlink(g_sockPath);
}

} // namespace shade::base
