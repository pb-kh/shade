#include "control.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shade::core
{

QJsonObject errorReply(const QString &message)
{
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

ControlServer::ControlServer(Handler handler, QObject *parent)
    : QObject(parent)
    , m_handler(std::move(handler))
{
}

ControlServer::~ControlServer() { close(); }

bool ControlServer::listen(const QString &path, QString *error)
{
    close();
    if (QFile::encodeName(path).size() >= 108) {
        if (error) *error = QStringLiteral("socket path too long (%1 bytes, max 107): %2").arg(QFile::encodeName(path).size()).arg(path);
        return false;
    }
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    // A leftover socket from an unclean exit: only remove it when nobody
    // answers (the pid lock already guarantees we are the only instance).
    if (QFile::exists(path)) {
        QLocalSocket probe;
        probe.connectToServer(path);
        if (probe.waitForConnected(150)) {
            if (error) *error = QStringLiteral("%1 is in use by another process").arg(path);
            return false;
        }
        QLocalServer::removeServer(path);
    }
    if (!m_server->listen(path)) {
        if (error) *error = m_server->errorString();
        return false;
    }
    ::chmod(QFile::encodeName(path).constData(), 0600);
    m_path = path;
    connect(m_server, &QLocalServer::newConnection, this, &ControlServer::onConnection);
    return true;
}

void ControlServer::close()
{
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
    if (!m_path.isEmpty()) {
        QFile::remove(m_path);
        m_path.clear();
    }
}

void ControlServer::onConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        // Peer credential check: only our own uid may talk to us.
        struct ucred cred{};
        socklen_t len = sizeof cred;
        const int fd = int(socket->socketDescriptor());
        if (fd < 0 || ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != ::getuid()) {
            socket->write(QJsonDocument(errorReply(QStringLiteral("unauthorized"))).toJson(QJsonDocument::Compact) + '\n');
            socket->disconnectFromServer();
            socket->deleteLater();
            continue;
        }
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
}

QJsonObject ControlServer::process(const QByteArray &line) const
{
    if (line.size() > kMaxRequest) return errorReply(QStringLiteral("request too large"));
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return errorReply(QStringLiteral("malformed request: expected a JSON object"));
    }
    const QJsonObject req = doc.object();
    if (!req.value(QStringLiteral("cmd")).isString()) return errorReply(QStringLiteral("missing \"cmd\""));
    QJsonObject reply = m_handler ? m_handler(req) : errorReply(QStringLiteral("no handler"));
    if (!reply.contains(QStringLiteral("ok"))) reply.insert(QStringLiteral("ok"), true);
    return reply;
}

void ControlServer::onReadyRead(QLocalSocket *socket)
{
    QByteArray buffer = socket->property("shade_buffer").toByteArray();
    buffer += socket->readAll();
    while (true) {
        const int nl = buffer.indexOf('\n');
        if (nl < 0) {
            if (buffer.size() > kMaxRequest) {
                socket->write(QJsonDocument(errorReply(QStringLiteral("request too large"))).toJson(QJsonDocument::Compact) + '\n');
                socket->disconnectFromServer();
                return;
            }
            break;
        }
        const QByteArray line = buffer.left(nl).trimmed();
        buffer.remove(0, nl + 1);
        if (line.isEmpty()) continue;
        const QJsonObject reply = process(line);
        socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
    }
    socket->setProperty("shade_buffer", buffer);
}

} // namespace shade::core
