#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QStringList>

namespace notif
{

enum Urgency : quint8 { Low = 0, Normal = 1, Critical = 2 };

struct Notification
{
    quint32 id = 0;
    QString app;
    QString icon;
    QString summary;
    QString body;
    QStringList actions; // key, label, key, label...
    quint8 urgency = Normal;
    qint32 expireTimeout = -1;
    QDateTime received;

    QString defaultAction() const { return actions.isEmpty() ? QString() : actions.first(); }
};

// The shared notification store (one per plugin, newest first). Entries
// never expire on their own: only a dismissal, a CloseNotification call or
// a replacement removes them.
class Model : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    const QList<Notification> &items() const { return m_items; }
    const Notification *find(quint32 id) const;
    bool dnd() const { return m_dnd; }
    void setDnd(bool on);

    // Inserts or replaces (keeping the position of a replaced entry).
    // Returns the id used.
    quint32 upsert(Notification n, quint32 replacesId);
    bool remove(quint32 id);
    QList<quint32> clear();

    static QString age(const QDateTime &received, const QDateTime &now = QDateTime::currentDateTime());
    static QString truncateBody(const QString &body);

signals:
    void added(quint32 id);
    void replaced(quint32 id);
    void removed(quint32 id);
    void cleared(const QList<quint32> &ids);
    void dndChanged(bool on);

private:
    QList<Notification> m_items;
    quint32 m_next = 1;
    bool m_dnd = false;
};

} // namespace notif
