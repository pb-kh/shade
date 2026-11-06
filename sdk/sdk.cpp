#include <shade/pane.h>
#include <shade/sdk.h>
#include <shade/theme.h>

#include <QApplication>
#include <QFontMetrics>
#include <QMutex>
#include <QPainter>

namespace shade
{

Bus::~Bus() = default;
Host::~Host() = default;

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

namespace
{
Theme &globalTheme()
{
    static Theme theme;
    return theme;
}
} // namespace

Theme::Theme()
{
    // flexoki-dark inspired defaults.
    const std::pair<const char *, const char *> defaults[] = {
        {"bg", "#000000"},     {"bg-2", "#100F0F"},   {"ui", "#1C1B1A"},
        {"ui-2", "#282726"},   {"ui-3", "#343331"},   {"tx-3", "#575653"},
        {"tx-2", "#878580"},   {"tx", "#CECDC3"},     {"red", "#D14D41"},
        {"orange", "#DA702C"}, {"yellow", "#D0A215"}, {"green", "#879A39"},
        {"cyan", "#3AA99F"},   {"blue", "#4385BE"},   {"purple", "#8B7EC8"},
        {"magenta", "#CE5D97"}, {"accent", "#4385BE"}, {"bar", "#000000"},
    };
    for (const auto &[name, value] : defaults) {
        m_colors.insert(QString::fromLatin1(name), QColor(QString::fromLatin1(value)));
    }
}

const Theme &Theme::get() { return globalTheme(); }
void Theme::set(const Theme &theme) { globalTheme() = theme; }

QColor Theme::color(const QString &name) const
{
    return m_colors.value(name, QColor(Qt::magenta));
}

void Theme::setColor(const QString &name, const QColor &color) { m_colors.insert(name, color); }

QStringList Theme::names() const
{
    QStringList n = m_colors.keys();
    n.sort();
    return n;
}

QFont Theme::font(int pixelSize, QFont::Weight weight)
{
    QFont f = QApplication::font();
    f.setPixelSize(pixelSize);
    f.setWeight(weight);
    return f;
}

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(lerp(a.redF(), b.redF(), t), lerp(a.greenF(), b.greenF(), t),
                            lerp(a.blueF(), b.blueF(), t), lerp(a.alphaF(), b.alphaF(), t));
}

QColor withAlpha(const QColor &c, qreal alpha)
{
    QColor out = c;
    out.setAlphaF(std::clamp(c.alphaF() * alpha, 0.0, 1.0));
    return out;
}

// ---------------------------------------------------------------------------
// PaneWidget
// ---------------------------------------------------------------------------

PaneWidget::PaneWidget(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);
    QPalette pal = palette();
    pal.setColor(QPalette::WindowText, Theme::get().tx());
    pal.setColor(QPalette::Text, Theme::get().tx());
    setPalette(pal);
}

PaneWidget::~PaneWidget() = default;

QSize PaneWidget::peekSizeHint(const PeekView &peek) const
{
    const QString text = peek.data.value(QStringLiteral("text")).toString();
    const QFontMetrics fm(Theme::font(13, QFont::DemiBold));
    return QSize(std::clamp(fm.horizontalAdvance(text) + 32, 120, 420), 34);
}

void PaneWidget::paintPeek(QPainter *painter, const QRectF &rect, const PeekView &peek, qreal)
{
    // Default mini presentation: the peek's "text", centered.
    painter->setPen(Theme::get().tx());
    painter->setFont(Theme::font(13, QFont::DemiBold));
    painter->drawText(rect, Qt::AlignCenter, peek.data.value(QStringLiteral("text")).toString());
}

bool PaneWidget::peekPointerEvent(QEvent::Type, const QPointF &, Qt::MouseButton, const PeekView &)
{
    return false;
}

void PaneWidget::hostSetMorph(qreal morph)
{
    if (qFuzzyCompare(morph + 1.0, m_morph + 1.0)) return;
    m_morph = morph;
    emit morphChanged(morph);
}

void PaneWidget::hostSetCornerRadius(qreal radius)
{
    if (qFuzzyCompare(radius, m_radius)) return;
    m_radius = radius;
    update();
}

QPainterPath PaneWidget::roundedRect() const
{
    QPainterPath path;
    const qreal r = std::min({m_radius, width() / 2.0, height() / 2.0});
    path.addRoundedRect(QRectF(rect()), r, r);
    return path;
}

void PaneWidget::hostSetActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    emit shadeActiveChanged(active);
}

bool splitCommand(const QString &command, QStringList *argv)
{
    QStringList out;
    QString cur;
    bool inWord = false;
    enum { None, Single, Double } quote = None;
    for (qsizetype i = 0; i < command.size(); ++i) {
        const QChar c = command.at(i);
        if (quote == Single) {
            if (c == u'\'') quote = None;
            else cur += c;
            continue;
        }
        if (quote == Double) {
            if (c == u'"') {
                quote = None;
            } else if (c == u'\\' && i + 1 < command.size() &&
                       QStringView(u"$`\"\\\n").contains(command.at(i + 1))) {
                if (command.at(i + 1) != u'\n') cur += command.at(i + 1); // backslash-newline: continuation
                ++i;
            } else {
                cur += c;
            }
            continue;
        }
        if (c == u'\\') {
            if (i + 1 >= command.size()) return false;
            if (command.at(i + 1) != u'\n') cur += command.at(i + 1);
            ++i;
            inWord = true;
        } else if (c == u'\'') {
            quote = Single;
            inWord = true;
        } else if (c == u'"') {
            quote = Double;
            inWord = true;
        } else if (c.isSpace()) {
            if (inWord) out << cur;
            cur.clear();
            inWord = false;
        } else {
            cur += c;
            inWord = true;
        }
    }
    if (quote != None) return false;
    if (inWord) out << cur;
    if (argv) *argv = out;
    return true;
}

} // namespace shade
