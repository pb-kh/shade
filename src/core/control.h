#pragma once

#include <QJsonObject>
#include <QObject>

#include <functional>

class QLocalServer;
class QLocalSocket;

namespace shade::core
{

// Local control endpoint: a Unix socket speaking newline-delimited JSON.
// Request: {"cmd": "...", ...}; response: {"ok": true, ...} or
// {"ok": false, "error": "..."}. Only the owning user may connect.
class ControlServer : public QObject
{
    Q_OBJECT

public:
    using Handler = std::function<QJsonObject(const QJsonObject &request)>;
    static constexpr int kMaxRequest = 64 * 1024;

    explicit ControlServer(Handler handler, QObject *parent = nullptr);
    ~ControlServer() override;

    bool listen(const QString &path, QString *error = nullptr);
    void close();
    QString path() const { return m_path; }

    // Exposed for tests: parse+dispatch one request line.
    QJsonObject process(const QByteArray &line) const;

private:
    void onConnection();
    void onReadyRead(QLocalSocket *socket);

    Handler m_handler;
    QLocalServer *m_server = nullptr;
    QString m_path;
};

QJsonObject errorReply(const QString &message);

} // namespace shade::core
