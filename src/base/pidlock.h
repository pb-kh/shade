#pragma once

#include <QString>

#include <sys/types.h>

namespace shade::base
{

// Strictly parse a PID file's content. Returns 0 when invalid (only 2..max).
pid_t parsePid(const QByteArray &content);
// Read the PID recorded at `path` (0 when missing/invalid).
pid_t readPid(const QString &path);
// True when `pid` names a live (non-zombie) process.
bool pidAlive(pid_t pid);

// Single-instance lock. acquire() returns:
//   Acquired      - we own the file now (removed again by release()).
//   AlreadyRunning- a live process holds it (runningPid()).
//   Error         - not a regular file / cannot create (error()).
class PidLock
{
public:
    enum Result { Acquired, AlreadyRunning, Error };

    explicit PidLock(QString path);
    ~PidLock();

    Result acquire();
    void release();
    pid_t runningPid() const { return m_running; }
    QString error() const { return m_error; }
    QString path() const { return m_path; }

private:
    QString m_path;
    int m_fd = -1;
    pid_t m_running = 0;
    QString m_error;
};

// Async-signal-safe cleanup for fatal signal handlers.
void installPidCleanup(const QString &path, const QString &socket);
void runPidCleanup();

} // namespace shade::base
