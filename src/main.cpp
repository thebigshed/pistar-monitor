#include "MainWindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("pistar-dmr");
    QApplication::setApplicationName("pistar-dmr");
    QApplication::setDesktopFileName("pistar-dmr");

    MainWindow w;
    w.show();
    return app.exec();
}
