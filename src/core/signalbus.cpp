#include "signalbus.h"

#include <QDebug>
#include <QMetaObject>
#include <QRegularExpression>

#include <exception>

namespace shade::core
{

Endpoint::Endpoint(SignalBus *bus, const QString &name)
    : m_bus(bus)
    , m_name(name)
{
}

Endpoint::~Endpoint() = default;

bool Endpoint::send(Signal signal)
{
    if (!m_bus) return false;
    return m_bus->post(m_name, std::move(signal));
}

int Endpoint::handle(const QString &name, Handler handler)
{
    if (!handler) return 0;
    const int id = m_nextId++;
    m_handlers.push_back({id, name, std::move(handler)});
    return id;
}

void Endpoint::unhandle(int handlerId)
{
    std::erase_if(m_handlers, [handlerId](const Entry &e) { return e.id == handlerId; });
}

void Endpoint::deliver(const Signal &signal)
{
    // Copy: a handler may (un)register handlers while we iterate.
    const std::vector<Entry> handlers = m_handlers;
    for (const Entry &e : handlers) {
        if (e.name != signal.name && e.name != QLatin1String("*")) continue;
        // A failing handler must never break the host.
        try {
            e.fn(signal);
        } catch (const std::exception &ex) {
            qWarning().noquote() << "shade: handler for" << signal.name << "in" << m_name << "threw:" << ex.what();
        } catch (...) {
            qWarning().noquote() << "shade: handler for" << signal.name << "in" << m_name << "threw";
        }
    }
}

SignalBus::SignalBus(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<shade::Signal>("shade::Signal");
}

SignalBus::~SignalBus()
{
    for (auto &e : m_endpoints) e->detach();
}

Endpoint *SignalBus::endpoint(const QString &name)
{
    auto it = m_endpoints.find(name);
    if (it != m_endpoints.end()) return it->get();
    auto ep = std::make_shared<Endpoint>(this, name);
    m_endpoints.insert(name, ep);
    return ep.get();
}

void SignalBus::removeEndpoint(const QString &name)
{
    auto it = m_endpoints.find(name);
    if (it == m_endpoints.end()) return;
    (*it)->detach();
    // Keep the object alive until queued deliveries referencing it drain.
    std::shared_ptr<Endpoint> keep = *it;
    m_endpoints.erase(it);
    QMetaObject::invokeMethod(this, [keep]() {}, Qt::QueuedConnection);
}

QStringList SignalBus::endpointNames() const
{
    QStringList n = m_endpoints.keys();
    n.sort();
    return n;
}

bool SignalBus::validName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z][A-Za-z0-9_-]{0,63}$"));
    return re.match(name).hasMatch();
}

void SignalBus::reject(const QString &why, const Signal &s)
{
    const QString msg = QStringLiteral("rejected signal '%1' %2 -> %3: %4").arg(s.name, s.source, s.destination, why);
    qWarning().noquote() << "shade:" << msg;
    m_diagnostics << msg;
    if (m_diagnostics.size() > 200) m_diagnostics.removeFirst();
}

bool SignalBus::post(const QString &from, Signal s)
{
    if (!s.source.isEmpty() && s.source != from) {
        reject(QStringLiteral("source forged by '%1'").arg(from), s);
        return false;
    }
    s.source = from;
    if (!validName(s.name)) {
        reject(QStringLiteral("bad name"), s);
        return false;
    }
    if (s.destination.isEmpty()) {
        reject(QStringLiteral("missing destination"), s);
        return false;
    }
    if (s.destination == QLatin1String(shade::kBroadcast)) {
        // Broadcast: every endpoint except the host and the sender.
        for (auto it = m_endpoints.cbegin(); it != m_endpoints.cend(); ++it) {
            if (it.key() == QLatin1String(shade::kHost) || it.key() == from) continue;
            dispatch(it.key(), s);
        }
        emit posted(s);
        return true;
    }
    if (!m_endpoints.contains(s.destination)) {
        reject(QStringLiteral("unknown destination"), s);
        return false;
    }
    dispatch(s.destination, s);
    emit posted(s);
    return true;
}

void SignalBus::dispatch(const QString &to, const Signal &s)
{
    // Deferred: delivered from the event loop, never re-entrantly.
    QMetaObject::invokeMethod(
        this,
        [this, to, s]() {
            auto it = m_endpoints.find(to);
            if (it == m_endpoints.end()) return; // removed meanwhile: drop
            std::shared_ptr<Endpoint> ep = *it;
            ++m_delivered;
            ep->deliver(s);
        },
        Qt::QueuedConnection);
}

} // namespace shade::core
