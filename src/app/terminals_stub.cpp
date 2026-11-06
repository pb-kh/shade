// Built without libvterm: terminal requests are rejected with a diagnostic.
#include "shadewindow.h"

namespace shade::app
{

struct ShadeWindow::TermSession
{
};

bool ShadeWindow::openTerminal(const QString &, const QStringList &, const QSize &, QString *error)
{
    if (error) *error = QStringLiteral("shade was built without terminal support (libvterm)");
    return false;
}
bool ShadeWindow::closeTerminal(const QString &, const QString &) { return false; }
void ShadeWindow::removeTerminals(const QString &) {}
bool ShadeWindow::hasTerminal(const QString &) const { return false; }
QJsonArray ShadeWindow::terminalsJson(bool) const { return {}; }
bool ShadeWindow::typeIntoTerminal(const QString &, const QString &) { return false; }
bool ShadeWindow::stepTerminals(qreal) { return false; }
void ShadeWindow::paintTerminals(QPainter &) {}
void ShadeWindow::retractAllTerminals(const QString &) {}
bool ShadeWindow::terminalPointer(QEvent::Type, const QPointF &, Qt::MouseButton, Qt::KeyboardModifiers) { return false; }
bool ShadeWindow::terminalWheel(const QPointF &, const QPointF &, Qt::KeyboardModifiers) { return false; }
bool ShadeWindow::terminalKey(QKeyEvent *) { return false; }
void ShadeWindow::terminalsFocusChanged(bool) {}
QRectF ShadeWindow::terminalsBounds() const { return {}; }
QSize ShadeWindow::terminalsCanvasNeed() const { return {}; }
void ShadeWindow::destroyTerminals() {}

} // namespace shade::app
