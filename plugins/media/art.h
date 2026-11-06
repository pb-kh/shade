#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;

namespace media
{

// Resolves and loads album art: file:// and absolute paths, http(s) (size
// and time limited, safe redirects only), data: URIs, and a YouTube
// thumbnail fallback derived from xesam:url when the player gives no art.
class ArtLoader : public QObject
{
    Q_OBJECT

public:
    explicit ArtLoader(QObject *parent = nullptr);

    // The URL actually used for a track (art URL or YouTube fallback).
    static QString resolve(const QString &artUrl, const QString &trackUrl);
    static QString youtubeId(const QString &url);

    // Starts loading `url` (cancels the previous request). `ready` fires
    // with a null image on failure or when `url` is empty.
    void load(const QString &url);
    QString current() const { return m_url; }

    static constexpr qint64 kMaxBytes = 12 * 1024 * 1024;
    static constexpr int kTimeoutMs = 8000;

signals:
    void ready(const QString &url, const QImage &image);

private:
    void finish(const QString &url, QImage image);

    QNetworkAccessManager *m_net = nullptr;
    QPointer<QNetworkReply> m_reply;
    QString m_url;
    QHash<QString, QImage> m_cache; // small LRU-ish cache
    QStringList m_order;
};

// Cheap, good-looking blur for a background: downscale hard, then upscale
// smoothly twice.
QImage blurredBackground(const QImage &src, const QSize &size);

} // namespace media
