#pragma once

#include <shade/sdk.h>

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <memory>

namespace shade::core
{

class SignalBus;

// One named endpoint. The bus stamps `source` with the endpoint's name; a
// signal claiming another source is rejected (no forging).
class Endpoint : public shade::Bus
{
public:
    Endpoint(SignalBus *bus, const QString &name);
    ~Endpoint() override;

    bool send(Signal signal) override;
    int handle(const QString &name, Handler handler) override;
    void unhandle(int handlerId) override;
    using shade::Bus::send;

    QString name() const { return m_name; }
    void deliver(const Signal &signal);
    int handlerCount() const { return int(m_handlers.size()); }
    void detach() { m_bus = nullptr; }

private:
    struct Entry
    {
        int id;
        QString name;
        Handler fn;
    };
    QPointer<SignalBus> m_bus;
    QString m_name;
    std::vector<Entry> m_handlers;
    int m_nextId = 1;
};

class SignalBus : public QObject
{
    Q_OBJECT

public:
    explicit SignalBus(QObject *parent = nullptr);
    ~SignalBus() override;

    // Creates (or returns) the endpoint for `name`. Owned by the bus.
    Endpoint *endpoint(const QString &name);
    // Drops an endpoint: queued signals to it are discarded and later sends
    // to it are rejected.
    void removeEndpoint(const QString &name);
    bool hasEndpoint(const QString &name) const { return m_endpoints.contains(name); }
    QStringList endpointNames() const;

    // Validate + queue. Called by endpoints; `from` is the sending endpoint.
    bool post(const QString &from, Signal signal);

    static bool validName(const QString &name);
    QStringList diagnostics() const { return m_diagnostics; }
    quint64 delivered() const { return m_delivered; }

signals:
    // Every accepted signal (after routing), for tracing/tests.
    void posted(const shade::Signal &signal);

private:
    void reject(const QString &why, const Signal &s);
    void dispatch(const QString &to, const Signal &s);

    QHash<QString, std::shared_ptr<Endpoint>> m_endpoints;
    QStringList m_diagnostics;
    quint64 m_delivered = 0;
};

} // namespace shade::core
