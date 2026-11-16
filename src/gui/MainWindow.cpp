#include "gui/MainWindow.h"

#include "viewport/Viewport.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QUrl>

namespace graph3d {
namespace {

QAction* menuAction(QMenu* menu, const QString& label, const char* name)
{
    auto* action = menu->addAction(label);
    action->setObjectName(QString::fromLatin1(name));
    return action;
}

}  // namespace

MainWindow::MainWindow()
    : viewport_(new Viewport(this))
{
    setWindowTitle(QStringLiteral("Graph3D"));
    resize(1040, 710);
    setMinimumSize(480, 320);
    setCentralWidget(viewport_);

    const auto folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!folder.isEmpty() && QDir().mkpath(folder)) {
        logPath_ = QDir(folder).filePath(QStringLiteral("graph3d.log"));
    }
    createMenus();
    connect(viewport_, &Viewport::sceneMessage, this, [this](const QString& message) {
        statusBar()->showMessage(message);
        writeLog(message);
    });
    connect(viewport_, &Viewport::operationFailed, this, &MainWindow::showSceneError);
    statusBar()->showMessage(tr("Starting 3D viewport…"));
}

bool MainWindow::initializeScene()
{
    if (!viewport_->initialize()) {
        return false;
    }
    writeLog(tr("3D viewport initialized"));
    viewport_->addPrimitive(geometry::Primitive::Cube);
    statusBar()->showMessage(tr("Left click: select   |   Right drag: orbit   |   Middle drag: pan   |   Wheel: zoom"));
    return true;
}

void MainWindow::createMenus()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    auto* newScene = menuAction(file, tr("&New scene"), "newScene");
    newScene->setShortcut(QKeySequence::New);
    connect(newScene, &QAction::triggered, viewport_, &Viewport::newScene);
    file->addSeparator();
    auto* quit = file->addAction(tr("E&xit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    auto* clear = menuAction(edit, tr("Clear selection"), "clearSelection");
    clear->setShortcut(Qt::Key_Escape);
    connect(clear, &QAction::triggered, viewport_, &Viewport::clearSelection);

    auto* create = menuBar()->addMenu(tr("&Create"));
    const auto add = [this, create](const QString& label, const char* name, geometry::Primitive kind) {
        auto* action = menuAction(create, label, name);
        connect(action, &QAction::triggered, viewport_, [this, kind] { viewport_->addPrimitive(kind); });
    };
    add(tr("Cube"), "createCube", geometry::Primitive::Cube);
    add(tr("Cylinder"), "createCylinder", geometry::Primitive::Cylinder);
    add(tr("Sphere"), "createSphere", geometry::Primitive::Sphere);

    auto* view = menuBar()->addMenu(tr("&View"));
    auto* fit = menuAction(view, tr("Fit all"), "fitScene");
    fit->setShortcut(Qt::Key_F);
    connect(fit, &QAction::triggered, viewport_, &Viewport::fitScene);
    const auto direction = [this, view](const QString& label, const char* name, ViewDirection orientation) {
        auto* action = menuAction(view, label, name);
        connect(action, &QAction::triggered, viewport_, [this, orientation] {
            viewport_->setDirection(orientation);
        });
    };
    direction(tr("Front"), "viewFront", ViewDirection::Front);
    direction(tr("Top"), "viewTop", ViewDirection::Top);
    direction(tr("Right"), "viewRight", ViewDirection::Right);
    direction(tr("Isometric"), "viewIsometric", ViewDirection::Isometric);
    view->addSeparator();
    auto* grid = menuAction(view, tr("Grid"), "showGrid");
    grid->setCheckable(true);
    grid->setChecked(true);
    connect(grid, &QAction::toggled, viewport_, &Viewport::setGridVisible);
    auto* axes = menuAction(view, tr("Axes"), "showAxes");
    axes->setCheckable(true);
    axes->setChecked(true);
    connect(axes, &QAction::toggled, viewport_, &Viewport::setAxesVisible);

    auto* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Mouse controls"), this, [this] {
        QMessageBox::information(this, tr("Mouse controls"),
            tr("Left click: select a body\nRight drag: orbit\nMiddle drag: pan\nWheel: zoom\nF: fit all"));
    });
    auto* openLog = help->addAction(tr("Open log folder"), this, [this] {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(logPath_).absolutePath()))) {
            statusBar()->showMessage(tr("Could not open the log folder"));
        }
    });
    openLog->setEnabled(!logPath_.isEmpty());
    help->addAction(tr("About Graph3D"), this, [this] {
        QMessageBox::about(this, tr("About Graph3D"), tr("Graph3D · Mechanical CAD prototype"));
    });
}

void MainWindow::writeLog(const QString& message)
{
    QFile log(logPath_);
    if (logPath_.isEmpty() || !log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qWarning("%s", qPrintable(message));
        return;
    }
    const auto time = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    log.write(QStringLiteral("%1 %2\n").arg(time, message).toUtf8());
}

void MainWindow::showSceneError(const QString& message)
{
    writeLog(tr("ERROR: %1").arg(message));
    qWarning("%s", qPrintable(message));
    statusBar()->showMessage(message);
}

}  // namespace graph3d
