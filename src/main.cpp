#include <QApplication>

#include "shadebar.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("shade"));
    QApplication::setApplicationDisplayName(QStringLiteral("shade"));
    QApplication::setDesktopFileName(QStringLiteral("shade"));

    ShadeBar bar;
    bar.showOnLayer();

    return app.exec();
}
