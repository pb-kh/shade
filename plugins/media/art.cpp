#include "art.h"

#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

namespace media
{

namespace
{
QImage decode(const QByteArray &bytes)
{
    QBuffer buf;
    buf.setData(bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader r(&buf);
    r.setAutoTransform(true);
    // Huge images are scaled while decoding.
    const QSize s = r.size();
    if (s.isValid() && (s.width() > 1024 || s.height() > 1024)) r.setScaledSize(s.scaled(1024, 1024, Qt::KeepAspectRatio));
    return r.read();
}
} // namespace

ArtLoader::ArtLoader(QObject *parent)
    : QObject(parent)
{
}

QString ArtLoader::youtubeId(const QString &url)
{
    static const QRegularExpression re(QStringLiteral(
        R"(^https?://(?:(?:www\.|m\.|music\.)?youtube\.com/(?:watch\?(?:.*&)?v=|shorts/|embed/)|youtu\.be/)([A-Za-z0-9_-]{11}))"));
    const auto m = re.match(url);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString ArtLoader::resolve(const QString &artUrl, const QString &trackUrl)
{
    if (!artUrl.isEmpty()) return artUrl;
    const QString id = youtubeId(trackUrl);
    if (!id.isEmpty()) return QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(id);
    return {};
}

void ArtLoader::load(const QString &url)
{
    if (url == m_url && (m_reply || m_cache.contains(url))) {
        if (!m_reply) emit ready(url, m_cache.value(url));
        return;
    }
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
    }
    m_url = url;
    if (url.isEmpty()) {
        emit ready(url, {});
        return;
    }
    if (m_cache.contains(url)) {
        emit ready(url, m_cache.value(url));
        return;
    }
    const QUrl u(url);
    if (url.startsWith(u'/') || u.isLocalFile()) {
        QFile f(url.startsWith(u'/') ? url : u.toLocalFile());
        if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxBytes) {
            finish(url, {});
            return;
        }
        finish(url, decode(f.readAll()));
        return;
    }
    if (u.scheme() == QLatin1String("data")) {
        const QString s = u.toString(QUrl::FullyEncoded);
        const int comma = s.indexOf(u',');
        if (comma < 0 || !s.left(comma).endsWith(QLatin1String(";base64"))) {
            finish(url, {});
            return;
        }
        finish(url, decode(QByteArray::fromBase64(s.mid(comma + 1).toLatin1())));
        return;
    }
    if (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https")) {
        finish(url, {});
        return;
    }
    if (!m_net) m_net = new QNetworkAccessManager(this);
    QNetworkRequest req(u);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(5);
    req.setTransferTimeout(kTimeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("shade"));
    QNetworkReply *reply = m_net->get(req);
    m_reply = reply;
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
        if (received > kMaxBytes || total > kMaxBytes) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
        reply->deleteLater();
        if (m_reply == reply) m_reply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            finish(url, {});
            return;
        }
        finish(url, decode(reply->readAll()));
    });
}

void ArtLoader::finish(const QString &url, QImage image)
{
    if (!image.isNull()) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        m_cache.insert(url, image);
        m_order.removeAll(url);
        m_order << url;
        while (m_order.size() > 8) m_cache.remove(m_order.takeFirst());
    }
    if (url == m_url) emit ready(url, image);
}

QImage blurredBackground(const QImage &src, const QSize &size)
{
    if (src.isNull() || size.isEmpty()) return {};
    // Cover-crop to the target aspect.
    const QSize cover = src.size().scaled(size, Qt::KeepAspectRatioByExpanding);
    QImage img = src.scaled(cover, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                     .copy(QRect(QPoint((cover.width() - size.width()) / 2, (cover.height() - size.height()) / 2), size));
    const QSize tiny(std::max(4, size.width() / 18), std::max(4, size.height() / 18));
    img = img.scaled(tiny, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    img = img.scaled(tiny * 3, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    img = img.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace media
