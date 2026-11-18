#include "gui/DocumentWindow.h"
#include "gui/InterfaceLanguage.h"

#include <QApplication>
#include <QIcon>
#include <QTimer>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Graph3D"));
    QApplication::setApplicationVersion(QStringLiteral(GRAPH3D_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/resources/icons/graph3d.png")));

    const graph3d::InterfaceLanguage language(graph3d::russianLanguagePreferred());
    graph3d::DocumentWindow window;
    window.show();
    if (app.arguments().size() > 1) {
        window.openDocument(app.arguments().at(1));
    }
    QTimer::singleShot(0, &window, &graph3d::DocumentWindow::offerRecovery);
    return app.exec();
}
