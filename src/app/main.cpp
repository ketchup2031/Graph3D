#include "gui/MainWindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Graph3D"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/resources/icons/graph3d.png")));

    graph3d::MainWindow window;
    window.show();
    return app.exec();
}
