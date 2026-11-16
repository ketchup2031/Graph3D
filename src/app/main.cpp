#include "gui/MainWindow.h"
#include "gui/InterfaceLanguage.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Graph3D"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/resources/icons/graph3d.png")));

    const graph3d::InterfaceLanguage language(graph3d::russianLanguagePreferred());
    graph3d::MainWindow window;
    window.show();
    return app.exec();
}
