#include "gui/MainWindow.h"

#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QStatusBar>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Graph3D"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/resources/icons/graph3d.png")));

    graph3d::MainWindow window;
    window.show();
    if (!window.initializeScene()) {
        QMessageBox::critical(&window, QStringLiteral("Graph3D"), window.statusBar()->currentMessage());
        return 1;
    }
    return app.exec();
}
