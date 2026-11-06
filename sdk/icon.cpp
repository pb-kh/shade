#include <shade/icon.h>
#include <shade/theme.h>

#include <QCache>
#include <QDebug>
#include <QFile>
#include <QHash>
#include <QMouseEvent>
#include <QMutex>
#include <QPainter>
#include <QRegularExpression>
#include <QTimer>
#include <QTransform>
#include <QXmlStreamReader>

#include <cmath>
#include <limits>

namespace shade
{

namespace
{

// ---------------------------------------------------------------------------
// SVG path data parser
// ---------------------------------------------------------------------------

class PathParser
{
public:
    explicit PathParser(const QString &d) : m_d(d) {}

    // Returns subpaths; `closed` receives one flag per subpath.
    QVector<QPainterPath> parse(QVector<bool> &closed)
    {
        QVector<QPainterPath> out;
        QPainterPath current;
        bool haveCurrent = false;
        bool currentClosed = false;
        QPointF pos, start, lastCtrl;
        QChar lastCmd;

        auto flush = [&]() {
            if (haveCurrent && current.elementCount() > 1) {
                out.push_back(current);
                closed.push_back(currentClosed);
            }
            current = QPainterPath();
            haveCurrent = false;
            currentClosed = false;
        };

        QChar cmd;
        while (true) {
            skipSeparators();
            if (atEnd()) {
                break;
            }
            const QChar c = m_d.at(m_i);
            if (c.isLetter()) {
                cmd = c;
                ++m_i;
            } else if (cmd.isNull()) {
                return {}; // number before any command
            } else if (cmd == u'M') {
                cmd = u'L';
            } else if (cmd == u'm') {
                cmd = u'l';
            }

            const bool rel = cmd.isLower();
            const QChar up = cmd.toUpper();
            bool ok = true;

            if (up == u'Z') {
                if (haveCurrent) {
                    current.closeSubpath();
                    currentClosed = true;
                }
                pos = start;
                flush();
                // A command after Z without M continues from start.
                lastCmd = cmd;
                cmd = QChar();
                continue;
            }

            if (up == u'M') {
                const QPointF p = point(rel, pos, ok);
                if (!ok) return {};
                flush();
                current.moveTo(p);
                haveCurrent = true;
                pos = start = p;
            } else {
                if (!haveCurrent) {
                    current.moveTo(pos);
                    start = pos;
                    haveCurrent = true;
                }
                if (up == u'L') {
                    pos = point(rel, pos, ok);
                    current.lineTo(pos);
                } else if (up == u'H') {
                    const qreal x = number(ok);
                    pos.setX(rel ? pos.x() + x : x);
                    current.lineTo(pos);
                } else if (up == u'V') {
                    const qreal y = number(ok);
                    pos.setY(rel ? pos.y() + y : y);
                    current.lineTo(pos);
                } else if (up == u'C') {
                    const QPointF c1 = point(rel, pos, ok);
                    const QPointF c2 = point(rel, pos, ok);
                    const QPointF e = point(rel, pos, ok);
                    current.cubicTo(c1, c2, e);
                    lastCtrl = c2;
                    pos = e;
                } else if (up == u'S') {
                    const QChar lu = lastCmd.toUpper();
                    const QPointF c1 = (lu == u'C' || lu == u'S') ? 2 * pos - lastCtrl : pos;
                    const QPointF c2 = point(rel, pos, ok);
                    const QPointF e = point(rel, pos, ok);
                    current.cubicTo(c1, c2, e);
                    lastCtrl = c2;
                    pos = e;
                } else if (up == u'Q') {
                    const QPointF c = point(rel, pos, ok);
                    const QPointF e = point(rel, pos, ok);
                    current.quadTo(c, e);
                    lastCtrl = c;
                    pos = e;
                } else if (up == u'T') {
                    const QChar lu = lastCmd.toUpper();
                    const QPointF c = (lu == u'Q' || lu == u'T') ? 2 * pos - lastCtrl : pos;
                    const QPointF e = point(rel, pos, ok);
                    current.quadTo(c, e);
                    lastCtrl = c;
                    pos = e;
                } else if (up == u'A') {
                    const qreal rx = number(ok);
                    const qreal ry = number(ok);
                    const qreal rot = number(ok);
                    const bool large = flag(ok);
                    const bool sweep = flag(ok);
                    const QPointF e = point(rel, pos, ok);
                    if (ok) arcTo(current, pos, e, rx, ry, rot, large, sweep);
                    pos = e;
                } else {
                    return {};
                }
            }
            if (!ok) {
                return {};
            }
            lastCmd = cmd;
        }
        flush();
        return out;
    }

private:
    bool atEnd() const { return m_i >= m_d.size(); }

    void skipSeparators()
    {
        while (!atEnd() && (m_d.at(m_i).isSpace() || m_d.at(m_i) == u',')) ++m_i;
    }

    qreal number(bool &ok)
    {
        skipSeparators();
        const int begin = m_i;
        if (!atEnd() && (m_d.at(m_i) == u'-' || m_d.at(m_i) == u'+')) ++m_i;
        bool dot = false, digits = false;
        while (!atEnd()) {
            const QChar c = m_d.at(m_i);
            if (c.isDigit()) { digits = true; ++m_i; }
            else if (c == u'.' && !dot) { dot = true; ++m_i; }
            else break;
        }
        if (digits && !atEnd() && (m_d.at(m_i) == u'e' || m_d.at(m_i) == u'E')) {
            int j = m_i + 1;
            if (j < m_d.size() && (m_d.at(j) == u'-' || m_d.at(j) == u'+')) ++j;
            if (j < m_d.size() && m_d.at(j).isDigit()) {
                m_i = j;
                while (!atEnd() && m_d.at(m_i).isDigit()) ++m_i;
            }
        }
        if (!digits) { ok = false; return 0; }
        bool conv = false;
        const qreal v = QStringView(m_d).mid(begin, m_i - begin).toDouble(&conv);
        if (!conv) ok = false;
        return v;
    }

    bool flag(bool &ok)
    {
        skipSeparators();
        if (atEnd()) { ok = false; return false; }
        const QChar c = m_d.at(m_i++);
        if (c == u'0') return false;
        if (c == u'1') return true;
        ok = false;
        return false;
    }

    QPointF point(bool rel, const QPointF &pos, bool &ok)
    {
        const qreal x = number(ok);
        const qreal y = number(ok);
        return rel ? QPointF(pos.x() + x, pos.y() + y) : QPointF(x, y);
    }

    // SVG arc (endpoint parameterization) -> cubic beziers.
    static void arcTo(QPainterPath &path, QPointF p0, QPointF p1, qreal rx, qreal ry,
                      qreal angleDeg, bool large, bool sweep)
    {
        rx = std::abs(rx);
        ry = std::abs(ry);
        if (rx < 1e-9 || ry < 1e-9 || p0 == p1) {
            path.lineTo(p1);
            return;
        }
        const qreal phi = qDegreesToRadians(angleDeg);
        const qreal cp = std::cos(phi), sp = std::sin(phi);
        const qreal dx = (p0.x() - p1.x()) / 2, dy = (p0.y() - p1.y()) / 2;
        const qreal x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
        qreal lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lambda > 1) { const qreal s = std::sqrt(lambda); rx *= s; ry *= s; }
        const qreal num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const qreal den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        qreal coef = den > 0 ? std::sqrt(std::max<qreal>(0, num / den)) : 0;
        if (large == sweep) coef = -coef;
        const qreal cxp = coef * rx * y1 / ry, cyp = -coef * ry * x1 / rx;
        const qreal cx = cp * cxp - sp * cyp + (p0.x() + p1.x()) / 2;
        const qreal cy = sp * cxp + cp * cyp + (p0.y() + p1.y()) / 2;
        auto angle = [](qreal ux, qreal uy, qreal vx, qreal vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const qreal theta1 = angle(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
        qreal dtheta = angle((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
        if (!sweep && dtheta > 0) dtheta -= 2 * M_PI;
        if (sweep && dtheta < 0) dtheta += 2 * M_PI;
        const int segs = std::max(1, static_cast<int>(std::ceil(std::abs(dtheta) / (M_PI / 2) - 1e-9)));
        const qreal delta = dtheta / segs;
        const qreal t = 4.0 / 3.0 * std::tan(delta / 4);
        qreal th = theta1;
        auto at = [&](qreal a, qreal &x, qreal &y, qreal &dxv, qreal &dyv) {
            const qreal ca = std::cos(a), sa = std::sin(a);
            x = cx + rx * ca * cp - ry * sa * sp;
            y = cy + rx * ca * sp + ry * sa * cp;
            dxv = -rx * sa * cp - ry * ca * sp;
            dyv = -rx * sa * sp + ry * ca * cp;
        };
        for (int i = 0; i < segs; ++i) {
            qreal xa, ya, dxa, dya, xb, yb, dxb, dyb;
            at(th, xa, ya, dxa, dya);
            at(th + delta, xb, yb, dxb, dyb);
            path.cubicTo(QPointF(xa + t * dxa, ya + t * dya), QPointF(xb - t * dxb, yb - t * dyb),
                         i == segs - 1 ? p1 : QPointF(xb, yb));
            th += delta;
        }
    }

    QString m_d;
    int m_i = 0;
};

// ---------------------------------------------------------------------------
// SVG document reader
// ---------------------------------------------------------------------------

struct Style
{
    bool hasFill = true;
    bool hasStroke = false;
    qreal strokeWidth = 1.0;
    qreal opacity = 1.0;
    qreal fillOpacity = 1.0;
    qreal strokeOpacity = 1.0;
    Qt::PenCapStyle cap = Qt::FlatCap;
    Qt::PenJoinStyle join = Qt::MiterJoin;
    Qt::FillRule fillRule = Qt::WindingFill;
    QString colorRole;
    QTransform transform;
    bool display = true;
};

bool isNone(const QString &v)
{
    const QString s = v.trimmed();
    return s.isEmpty() || s == QLatin1String("none") || s == QLatin1String("transparent");
}

QVector<qreal> numbers(const QString &s)
{
    QVector<qreal> out;
    static const QRegularExpression re(QStringLiteral("[-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?"));
    auto it = re.globalMatch(s);
    while (it.hasNext()) out.push_back(it.next().captured(0).toDouble());
    return out;
}

QTransform parseTransform(const QString &s)
{
    QTransform result;
    static const QRegularExpression re(QStringLiteral("(\\w+)\\s*\\(([^)]*)\\)"));
    auto it = re.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString kind = m.captured(1);
        const QVector<qreal> n = numbers(m.captured(2));
        QTransform t;
        if (kind == QLatin1String("translate") && !n.isEmpty()) {
            t.translate(n[0], n.size() > 1 ? n[1] : 0);
        } else if (kind == QLatin1String("scale") && !n.isEmpty()) {
            t.scale(n[0], n.size() > 1 ? n[1] : n[0]);
        } else if (kind == QLatin1String("rotate") && !n.isEmpty()) {
            if (n.size() >= 3) { t.translate(n[1], n[2]); t.rotate(n[0]); t.translate(-n[1], -n[2]); }
            else t.rotate(n[0]);
        } else if (kind == QLatin1String("matrix") && n.size() == 6) {
            t = QTransform(n[0], n[1], n[2], n[3], n[4], n[5]);
        }
        // SVG applies the list right-to-left: later transforms are inner.
        result = t * result;
    }
    return result;
}

void applyProperty(Style &st, const QString &name, const QString &value)
{
    const QString v = value.trimmed();
    if (name == QLatin1String("fill")) st.hasFill = !isNone(v);
    else if (name == QLatin1String("stroke")) st.hasStroke = !isNone(v);
    else if (name == QLatin1String("stroke-width")) st.strokeWidth = numbers(v).value(0, st.strokeWidth);
    else if (name == QLatin1String("opacity")) st.opacity *= numbers(v).value(0, 1.0);
    else if (name == QLatin1String("fill-opacity")) st.fillOpacity = numbers(v).value(0, 1.0);
    else if (name == QLatin1String("stroke-opacity")) st.strokeOpacity = numbers(v).value(0, 1.0);
    else if (name == QLatin1String("stroke-linecap"))
        st.cap = v == QLatin1String("round") ? Qt::RoundCap : v == QLatin1String("square") ? Qt::SquareCap : Qt::FlatCap;
    else if (name == QLatin1String("stroke-linejoin"))
        st.join = v == QLatin1String("round") ? Qt::RoundJoin : v == QLatin1String("bevel") ? Qt::BevelJoin : Qt::MiterJoin;
    else if (name == QLatin1String("fill-rule"))
        st.fillRule = v == QLatin1String("evenodd") ? Qt::OddEvenFill : Qt::WindingFill;
    else if (name == QLatin1String("data-color")) st.colorRole = v;
    else if (name == QLatin1String("display")) st.display = v != QLatin1String("none");
}

Style childStyle(const Style &parent, const QXmlStreamAttributes &attrs)
{
    Style st = parent;
    st.transform = QTransform();
    static const char *props[] = {"fill", "stroke", "stroke-width", "opacity", "fill-opacity",
                                  "stroke-opacity", "stroke-linecap", "stroke-linejoin",
                                  "fill-rule", "data-color", "display"};
    for (const char *p : props) {
        const QString name = QString::fromLatin1(p);
        if (attrs.hasAttribute(name)) applyProperty(st, name, attrs.value(name).toString());
    }
    // style="a:b;c:d" overrides presentation attributes.
    const QString style = attrs.value(QStringLiteral("style")).toString();
    for (const QString &decl : style.split(u';', Qt::SkipEmptyParts)) {
        const int colon = decl.indexOf(u':');
        if (colon > 0) applyProperty(st, decl.left(colon).trimmed(), decl.mid(colon + 1));
    }
    const QTransform own = parseTransform(attrs.value(QStringLiteral("transform")).toString());
    st.transform = own * parent.transform;
    return st;
}

qreal attr(const QXmlStreamAttributes &a, const char *name, qreal fallback = 0)
{
    const QString v = a.value(QString::fromLatin1(name)).toString();
    if (v.isEmpty()) return fallback;
    return numbers(v).value(0, fallback);
}

QVector<QPointF> samplePath(const QPainterPath &path, bool closed)
{
    const int n = VectorIcon::kSamples;
    QVector<QPointF> pts;
    pts.reserve(n);
    const qreal length = path.length();
    if (length <= 1e-9) {
        const QPointF p = path.elementCount() ? QPointF(path.elementAt(0)) : QPointF();
        pts.fill(p, n);
        return pts;
    }
    for (int i = 0; i < n; ++i) {
        const qreal t = closed ? qreal(i) / n : qreal(i) / (n - 1);
        pts.push_back(path.pointAtPercent(path.percentAtLength(t * length)));
    }
    return pts;
}

QPointF centroid(const QVector<QPointF> &pts)
{
    QPointF c;
    for (const QPointF &p : pts) c += p;
    return pts.isEmpty() ? c : c / pts.size();
}

} // namespace

VectorIcon VectorIcon::fromSvg(const QByteArray &svg, const QString &origin)
{
    VectorIcon icon;
    QXmlStreamReader xml(svg);
    // Icon editors emit undeclared prefixes (e.g. gpa:state); tolerate them.
    xml.setNamespaceProcessing(false);
    QVector<Style> stack;
    stack.push_back(Style());
    int anonymous = 0;
    bool sawRoot = false;
    int skipDepth = 0;

    auto addElement = [&](const QString &id, const QVector<QPainterPath> &paths,
                          const QVector<bool> &closed, const Style &st, const QString &anchorAttr,
                          const QString &group) {
        if (!st.display) return;
        const QString base = id.isEmpty() ? QStringLiteral("e%1").arg(anonymous++) : id;
        QPointF anchor;
        bool haveAnchor = false;
        const QVector<qreal> a = numbers(anchorAttr);
        if (a.size() >= 2) {
            anchor = st.transform.map(QPointF(a[0], a[1]));
            haveAnchor = true;
        }
        for (int i = 0; i < paths.size(); ++i) {
            IconContour c;
            c.key = i == 0 ? base : base + QStringLiteral("#%1").arg(i);
            c.path = st.transform.map(paths[i]);
            c.closed = closed.value(i);
            c.points = samplePath(c.path, c.closed);
            c.anchor = haveAnchor ? anchor : centroid(c.points);
            const qreal scale = std::sqrt(std::abs(st.transform.determinant()));
            c.strokeWidth = st.hasStroke ? st.strokeWidth * (scale > 0 ? scale : 1) : 0.0;
            c.strokeOpacity = st.strokeOpacity * st.opacity;
            c.fillOpacity = st.hasFill ? st.fillOpacity * st.opacity : 0.0;
            c.cap = st.cap;
            c.join = st.join;
            c.colorRole = st.colorRole;
            c.path.setFillRule(st.fillRule);
            c.path.setFillRule(st.fillRule);
            // Group key: subpaths of one element fill together (holes).
            c.colorRole = st.colorRole;
            Q_UNUSED(group);
            icon.contours.push_back(c);
        }
    };

    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = xml.name().toString();
            const auto attrs = xml.attributes();
            if (skipDepth > 0) { ++skipDepth; continue; }
            if (name == QLatin1String("metadata") || name == QLatin1String("defs") ||
                name == QLatin1String("title") || name == QLatin1String("desc") ||
                name == QLatin1String("clipPath") || name == QLatin1String("mask")) {
                skipDepth = 1;
                continue;
            }
            Style st = childStyle(stack.back(), attrs);
            stack.push_back(st);
            const QString id = attrs.value(QStringLiteral("id")).toString();
            const QString anchor = attrs.value(QStringLiteral("data-anchor")).toString();

            if (name == QLatin1String("svg")) {
                if (!sawRoot) {
                    sawRoot = true;
                    const QVector<qreal> vb = numbers(attrs.value(QStringLiteral("viewBox")).toString());
                    if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) {
                        icon.viewBox = QRectF(vb[0], vb[1], vb[2], vb[3]);
                    } else {
                        const qreal w = attr(attrs, "width", 16), h = attr(attrs, "height", 16);
                        icon.viewBox = QRectF(0, 0, w, h);
                    }
                }
            } else if (name == QLatin1String("path")) {
                QVector<bool> closed;
                PathParser parser(attrs.value(QStringLiteral("d")).toString());
                const auto paths = parser.parse(closed);
                addElement(id, paths, closed, st, anchor, id);
            } else if (name == QLatin1String("circle") || name == QLatin1String("ellipse")) {
                const qreal cx = attr(attrs, "cx"), cy = attr(attrs, "cy");
                const qreal rx = name == QLatin1String("circle") ? attr(attrs, "r") : attr(attrs, "rx");
                const qreal ry = name == QLatin1String("circle") ? rx : attr(attrs, "ry");
                QPainterPath p;
                // Start at the top so circles morph predictably.
                p.moveTo(cx, cy - ry);
                p.arcTo(QRectF(cx - rx, cy - ry, 2 * rx, 2 * ry), 90, -360);
                p.closeSubpath();
                addElement(id, {p}, {true}, st, anchor, id);
            } else if (name == QLatin1String("rect")) {
                const qreal x = attr(attrs, "x"), y = attr(attrs, "y");
                const qreal w = attr(attrs, "width"), h = attr(attrs, "height");
                qreal rx = attr(attrs, "rx", -1), ry = attr(attrs, "ry", -1);
                if (rx < 0) rx = ry < 0 ? 0 : ry;
                if (ry < 0) ry = rx;
                QPainterPath p;
                if (rx > 0 || ry > 0) p.addRoundedRect(QRectF(x, y, w, h), rx, ry);
                else p.addRect(QRectF(x, y, w, h));
                addElement(id, {p}, {true}, st, anchor, id);
            } else if (name == QLatin1String("line")) {
                QPainterPath p;
                p.moveTo(attr(attrs, "x1"), attr(attrs, "y1"));
                p.lineTo(attr(attrs, "x2"), attr(attrs, "y2"));
                Style ls = st;
                ls.hasFill = false;
                addElement(id, {p}, {false}, ls, anchor, id);
            } else if (name == QLatin1String("polyline") || name == QLatin1String("polygon")) {
                const QVector<qreal> n = numbers(attrs.value(QStringLiteral("points")).toString());
                QPainterPath p;
                for (int i = 0; i + 1 < n.size(); i += 2) {
                    if (i == 0) p.moveTo(n[i], n[i + 1]);
                    else p.lineTo(n[i], n[i + 1]);
                }
                const bool closed = name == QLatin1String("polygon");
                if (closed) p.closeSubpath();
                addElement(id, {p}, {closed}, st, anchor, id);
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (skipDepth > 0) { --skipDepth; continue; }
            if (stack.size() > 1) stack.pop_back();
        }
    }
    if (xml.hasError() || !sawRoot) {
        qWarning().noquote() << "shade: invalid SVG" << origin << xml.errorString();
        return {};
    }
    return icon;
}

QSharedPointer<const VectorIcon> VectorIcon::load(const QString &path)
{
    static QMutex mutex;
    static QHash<QString, QSharedPointer<const VectorIcon>> cache;
    QMutexLocker lock(&mutex);
    auto it = cache.constFind(path);
    if (it != cache.constEnd()) return *it;
    QFile file(path);
    QSharedPointer<const VectorIcon> icon;
    if (file.open(QIODevice::ReadOnly)) {
        icon = QSharedPointer<const VectorIcon>::create(VectorIcon::fromSvg(file.readAll(), path));
    } else {
        qWarning().noquote() << "shade: cannot open icon" << path;
        icon = QSharedPointer<const VectorIcon>::create();
    }
    cache.insert(path, icon);
    return icon;
}

// ---------------------------------------------------------------------------
// Morphing
// ---------------------------------------------------------------------------

namespace
{

// Re-order `to` so it lines up with `from` (best rotation for closed
// contours, best direction for both).
QVector<QPointF> align(const QVector<QPointF> &from, const QVector<QPointF> &to, bool closed)
{
    const int n = from.size();
    if (n == 0 || to.size() != n) return to;
    auto cost = [&](const QVector<QPointF> &cand, int shift) {
        qreal sum = 0;
        for (int i = 0; i < n; i += 2) {
            const QPointF d = from[i] - cand[(i + shift) % n];
            sum += d.x() * d.x() + d.y() * d.y();
        }
        return sum;
    };
    QVector<QPointF> reversed(to.rbegin(), to.rend());
    if (!closed) {
        return cost(reversed, 0) < cost(to, 0) ? reversed : to;
    }
    qreal best = std::numeric_limits<qreal>::max();
    const QVector<QPointF> *bestSet = &to;
    int bestShift = 0;
    for (const QVector<QPointF> *cand : std::initializer_list<const QVector<QPointF> *>{&to, &reversed}) {
        for (int s = 0; s < n; ++s) {
            const qreal c = cost(*cand, s);
            if (c < best) { best = c; bestSet = cand; bestShift = s; }
        }
    }
    QVector<QPointF> out(n);
    for (int i = 0; i < n; ++i) out[i] = (*bestSet)[(i + bestShift) % n];
    return out;
}

IconContour collapsed(const IconContour &c, const QPointF &anchor)
{
    IconContour out = c;
    out.points.fill(anchor, c.points.size());
    out.path = QPainterPath();
    out.strokeOpacity = 0;
    out.fillOpacity = 0;
    out.strokeWidth = 0;
    return out;
}

struct Pair
{
    IconContour from;
    IconContour to;
};

QVector<Pair> pairUp(const VectorIcon &a, const VectorIcon &b)
{
    QVector<Pair> pairs;
    QHash<QString, int> inB;
    for (int i = 0; i < b.contours.size(); ++i) inB.insert(b.contours[i].key, i);
    QSet<int> used;
    for (const IconContour &ca : a.contours) {
        const int j = inB.value(ca.key, -1);
        if (j >= 0) {
            used.insert(j);
            IconContour cb = b.contours[j];
            cb.points = align(ca.points, cb.points, ca.closed && cb.closed);
            pairs.push_back({ca, cb});
        } else {
            pairs.push_back({ca, collapsed(ca, ca.anchor)});
        }
    }
    for (int j = 0; j < b.contours.size(); ++j) {
        if (!used.contains(j)) pairs.push_back({collapsed(b.contours[j], b.contours[j].anchor), b.contours[j]});
    }
    return pairs;
}

VectorIcon lerpPairs(const QVector<Pair> &pairs, const QRectF &vbA, const QRectF &vbB, qreal t)
{
    VectorIcon out;
    out.viewBox = QRectF(lerp(vbA.x(), vbB.x(), t), lerp(vbA.y(), vbB.y(), t),
                         lerp(vbA.width(), vbB.width(), t), lerp(vbA.height(), vbB.height(), t));
    out.contours.reserve(pairs.size());
    for (const Pair &p : pairs) {
        IconContour c = t < 0.5 ? p.from : p.to;
        const int n = std::min(p.from.points.size(), p.to.points.size());
        c.points.resize(n);
        for (int i = 0; i < n; ++i) {
            c.points[i] = p.from.points[i] + (p.to.points[i] - p.from.points[i]) * t;
        }
        c.closed = p.from.closed && p.to.closed;
        c.path = QPainterPath();
        // Appearing/disappearing contours fade over the second/first half
        // so they visibly grow out of (or shrink into) their anchor.
        c.strokeWidth = lerp(p.from.strokeWidth > 0 ? p.from.strokeWidth : p.to.strokeWidth * 0.2,
                             p.to.strokeWidth > 0 ? p.to.strokeWidth : p.from.strokeWidth * 0.2, t);
        if (p.from.strokeWidth <= 0 && p.to.strokeWidth <= 0) c.strokeWidth = 0;
        c.strokeOpacity = lerp(p.from.strokeOpacity, p.to.strokeOpacity, t);
        c.fillOpacity = lerp(p.from.fillOpacity, p.to.fillOpacity, t);
        if (p.from.strokeWidth <= 0 && p.to.strokeWidth > 0) c.strokeOpacity = p.to.strokeOpacity * t;
        if (p.to.strokeWidth <= 0 && p.from.strokeWidth > 0) c.strokeOpacity = p.from.strokeOpacity * (1 - t);
        c.anchor = p.from.anchor + (p.to.anchor - p.from.anchor) * t;
        out.contours.push_back(c);
    }
    return out;
}

} // namespace

VectorIcon interpolateIcons(const VectorIcon &from, const VectorIcon &to, qreal t)
{
    if (t <= 0) return from;
    if (t >= 1) return to;
    return lerpPairs(pairUp(from, to), from.viewBox, to.viewBox, t);
}

void paintIcon(QPainter *painter, const VectorIcon &icon, const QRectF &target, const QColor &color, bool exact)
{
    if (icon.isNull() || target.isEmpty()) return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const QRectF vb = icon.viewBox;
    const qreal scale = std::min(target.width() / vb.width(), target.height() / vb.height());
    const QPointF offset = target.center() - QPointF(vb.width() * scale / 2, vb.height() * scale / 2);
    QTransform t;
    t.translate(offset.x(), offset.y());
    t.scale(scale, scale);
    t.translate(-vb.x(), -vb.y());
    painter->setTransform(t, true);

    // Subpaths that belong to one element fill together (keeps holes).
    QHash<QString, QPainterPath> fills;
    QHash<QString, std::pair<QColor, qreal>> fillStyle;
    QStringList order;

    for (const IconContour &c : icon.contours) {
        QPainterPath path;
        if (exact && !c.path.isEmpty()) {
            path = c.path;
        } else if (!c.points.isEmpty()) {
            path.moveTo(c.points[0]);
            for (int i = 1; i < c.points.size(); ++i) path.lineTo(c.points[i]);
            if (c.closed) path.closeSubpath();
        }
        const QColor base = c.colorRole.isEmpty() ? color : Theme::get().color(c.colorRole);
        if (c.fillOpacity > 0.001) {
            const QString group = c.key.section(u'#', 0, 0) + u'|' + c.colorRole;
            if (!fills.contains(group)) {
                order << group;
                QPainterPath fp;
                fp.setFillRule(c.path.isEmpty() ? Qt::WindingFill : c.path.fillRule());
                fills.insert(group, fp);
                fillStyle.insert(group, {base, c.fillOpacity});
            }
            fills[group].addPath(path);
        }
        if (c.strokeWidth > 0.001 && c.strokeOpacity > 0.001) {
            QColor sc = base;
            sc.setAlphaF(base.alphaF() * std::clamp(c.strokeOpacity, 0.0, 1.0));
            QPen pen(sc, c.strokeWidth, Qt::SolidLine, c.cap, c.join);
            painter->strokePath(path, pen);
        }
    }
    for (const QString &g : order) {
        QColor fc = fillStyle[g].first;
        fc.setAlphaF(fc.alphaF() * std::clamp(fillStyle[g].second, 0.0, 1.0));
        painter->fillPath(fills[g], fc);
    }
    painter->restore();
}

// ---------------------------------------------------------------------------
// MorphIcon
// ---------------------------------------------------------------------------

MorphIcon::MorphIcon(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(16);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &MorphIcon::step);
}

MorphIcon::MorphIcon(const QString &source, QObject *parent)
    : MorphIcon(parent)
{
    setSource(source, false);
}

void MorphIcon::setSource(const QString &source, bool animate)
{
    if (source == m_source && m_target) return;
    QSharedPointer<const VectorIcon> next = source.isEmpty()
        ? QSharedPointer<const VectorIcon>::create()
        : VectorIcon::load(source);
    if (animate && m_target && m_duration > 0) {
        m_from = current();
        m_progress = 0.0;
        m_clock.start();
        m_timer->start();
    } else {
        m_progress = 1.0;
        m_timer->stop();
    }
    m_source = source;
    m_target = next;
    emit changed();
}

bool MorphIcon::isAnimating() const { return m_progress < 1.0; }

VectorIcon MorphIcon::current() const
{
    if (!m_target) return {};
    if (m_progress >= 1.0) return *m_target;
    return interpolateIcons(m_from, *m_target, m_easing.valueForProgress(m_progress));
}

void MorphIcon::step()
{
    m_progress = std::min<qreal>(1.0, m_clock.elapsed() / qreal(std::max(1, m_duration)));
    if (m_progress >= 1.0) m_timer->stop();
    emit changed();
}

void MorphIcon::paint(QPainter *painter, const QRectF &target, const QColor &color) const
{
    if (!m_target) return;
    if (m_progress >= 1.0) paintIcon(painter, *m_target, target, color, true);
    else paintIcon(painter, current(), target, color, false);
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

IconWidget::IconWidget(const QString &source, int iconSize, QWidget *parent)
    : QWidget(parent)
    , m_icon(new MorphIcon(source, this))
    , m_iconSize(iconSize)
{
    setAttribute(Qt::WA_TranslucentBackground);
    connect(m_icon, &MorphIcon::changed, this, qOverload<>(&QWidget::update));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void IconWidget::setColor(const QColor &color)
{
    m_color = color;
    update();
}

QColor IconWidget::color() const
{
    if (m_color.isValid()) return m_color;
    return isEnabled() ? Theme::get().tx() : withAlpha(Theme::get().tx(), 0.35);
}

void IconWidget::setIconSize(int size)
{
    m_iconSize = size;
    updateGeometry();
    update();
}

QSize IconWidget::sizeHint() const { return QSize(m_iconSize, m_iconSize); }

void IconWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRectF r(QPointF(0, 0), QSizeF(m_iconSize, m_iconSize));
    QRectF target = r;
    target.moveCenter(QRectF(rect()).center());
    m_icon->paint(&p, target, color());
}

IconButton::IconButton(const QString &source, int iconSize, QWidget *parent)
    : IconWidget(source, iconSize, parent)
{
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setInterval(16);
    connect(m_hoverTimer, &QTimer::timeout, this, [this]() {
        const qreal speed = 0.18;
        m_hover += (m_hoverTarget - m_hover) * speed * 1.6;
        const qreal pressTarget = m_pressed ? 1.0 : 0.0;
        m_press += (pressTarget - m_press) * 0.35;
        if (std::abs(m_hover - m_hoverTarget) < 0.01 && std::abs(m_press - pressTarget) < 0.01) {
            m_hover = m_hoverTarget;
            m_press = pressTarget;
            m_hoverTimer->stop();
        }
        update();
    });
}

QSize IconButton::sizeHint() const
{
    return QSize(m_iconSize + 2 * m_padding, m_iconSize + 2 * m_padding);
}

void IconButton::animateHover(qreal to)
{
    m_hoverTarget = to;
    m_hoverTimer->start();
}

void IconButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (m_hover > 0.01 && isEnabled()) {
        QColor bg = Theme::get().ui2();
        bg.setAlphaF(bg.alphaF() * m_hover);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        const qreal radius = std::min(r.width(), r.height()) / 2;
        p.drawRoundedRect(r, radius, radius);
    }
    const qreal s = m_iconSize * (1.0 - 0.12 * m_press);
    QRectF target(0, 0, s, s);
    target.moveCenter(r.center());
    m_icon->paint(&p, target, color());
}

void IconButton::enterEvent(QEnterEvent *) { animateHover(1.0); }
void IconButton::leaveEvent(QEvent *) { m_pressed = false; animateHover(0.0); }

void IconButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && isEnabled()) {
        m_pressed = true;
        m_hoverTimer->start();
        e->accept();
    } else {
        e->ignore();
    }
}

void IconButton::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) { e->ignore(); return; }
    const bool was = m_pressed;
    m_pressed = false;
    m_hoverTimer->start();
    e->accept();
    if (was && rect().contains(e->position().toPoint()) && isEnabled()) emit clicked();
}

void IconButton::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::EnabledChange) update();
    QWidget::changeEvent(e);
}

} // namespace shade
