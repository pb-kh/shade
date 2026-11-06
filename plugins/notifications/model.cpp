#include "model.h"

namespace notif
{

const Notification *Model::find(quint32 id) const
{
    for (const Notification &n : m_items) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

void Model::setDnd(bool on)
{
    if (on == m_dnd) return;
    m_dnd = on;
    emit dndChanged(on);
}

quint32 Model::upsert(Notification n, quint32 replacesId)
{
    if (!n.received.isValid()) n.received = QDateTime::currentDateTime();
    if (replacesId != 0) {
        for (Notification &e : m_items) {
            if (e.id != replacesId) continue;
            n.id = replacesId;
            e = n;
            emit replaced(replacesId);
            return replacesId;
        }
    }
    n.id = m_next++;
    if (m_next == 0) m_next = 1; // never hand out 0
    m_items.prepend(n);
    emit added(n.id);
    return n.id;
}

bool Model::remove(quint32 id)
{
    for (qsizetype i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id != id) continue;
        m_items.removeAt(i);
        emit removed(id);
        return true;
    }
    return false;
}

QList<quint32> Model::clear()
{
    QList<quint32> ids;
    for (const Notification &n : m_items) ids << n.id;
    m_items.clear();
    if (!ids.isEmpty()) emit cleared(ids);
    return ids;
}

QString Model::age(const QDateTime &received, const QDateTime &now)
{
    const qint64 s = std::max<qint64>(0, received.secsTo(now));
    if (s < 60) return QStringLiteral("now");
    if (s < 3600) return QStringLiteral("%1m").arg(s / 60);
    const qint64 halves = s / 1800; // half-hour buckets
    if (halves % 2 == 0) return QStringLiteral("%1h").arg(halves / 2);
    return QStringLiteral("%1.5h").arg(halves / 2);
}

QString Model::truncateBody(const QString &body)
{
    const QString b = body.trimmed();
    if (b.size() <= 200) return b;
    return b.left(200).trimmed() + QChar(0x2026);
}

} // namespace notif
