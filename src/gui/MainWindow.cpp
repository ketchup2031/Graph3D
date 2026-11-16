#include "gui/MainWindow.h"

#include "gui/SketchEditor.h"
#include "viewport/Viewport.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace graph3d {
namespace {

constexpr int objectIdRole = Qt::UserRole;
constexpr int sketchRole = Qt::UserRole + 1;

QAction* menuAction(QMenu* menu, const QString& label, const char* name)
{
    auto* action = menu->addAction(label);
    action->setObjectName(QString::fromLatin1(name));
    return action;
}

}  // namespace

MainWindow::MainWindow()
    : viewport_(new Viewport(this)), pages_(new QStackedWidget(this)),
      sketchEditor_(new SketchEditor(this)), tree_(new QTreeWidget(this))
{
    setWindowTitle(QStringLiteral("Graph3D"));
    resize(1040, 710);
    setMinimumSize(480, 320);
    setCentralWidget(pages_);
    pages_->addWidget(viewport_);
    pages_->addWidget(sketchEditor_);
    createModelTree();
    connect(sketchEditor_, &SketchEditor::finishRequested, this, &MainWindow::finishSketch);
    connect(sketchEditor_, &SketchEditor::cancelRequested, this, [this] {
        leaveSketch();
        statusBar()->showMessage(tr("Sketch cancelled"));
    });

    const auto folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!folder.isEmpty() && QDir().mkpath(folder)) {
        logPath_ = QDir(folder).filePath(QStringLiteral("graph3d.log"));
    }
    createMenus();
    updateActions();
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
    newScene_ = menuAction(file, tr("&New scene"), "newScene");
    newScene_->setShortcut(QKeySequence::New);
    connect(newScene_, &QAction::triggered, viewport_, &Viewport::newScene);
    file->addSeparator();
    auto* quit = file->addAction(tr("E&xit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    editMenu_ = menuBar()->addMenu(tr("&Edit"));
    auto* clear = menuAction(editMenu_, tr("Clear selection"), "clearSelection");
    clear->setShortcut(Qt::Key_Escape);
    connect(clear, &QAction::triggered, viewport_, &Viewport::clearSelection);

    createMenu_ = menuBar()->addMenu(tr("&Create"));
    const auto add = [this](const QString& label, const char* name, geometry::Primitive kind) {
        auto* action = menuAction(createMenu_, label, name);
        connect(action, &QAction::triggered, viewport_, [this, kind] { viewport_->addPrimitive(kind); });
    };
    add(tr("Cube"), "createCube", geometry::Primitive::Cube);
    add(tr("Cylinder"), "createCylinder", geometry::Primitive::Cylinder);
    add(tr("Sphere"), "createSphere", geometry::Primitive::Sphere);

    sketchMenu_ = menuBar()->addMenu(tr("&Sketch"));
    const auto planeAction = [this](const QString& label, const char* name, geometry::Plane plane) {
        auto* action = menuAction(sketchMenu_, label, name);
        connect(action, &QAction::triggered, this, [this, plane] { beginSketch(plane); });
    };
    planeAction(tr("New on XY"), "newSketchXY", geometry::Plane::XY);
    planeAction(tr("New on XZ"), "newSketchXZ", geometry::Plane::XZ);
    planeAction(tr("New on YZ"), "newSketchYZ", geometry::Plane::YZ);

    auto* model = menuBar()->addMenu(tr("&Model"));
    extrude_ = menuAction(model, tr("Extrude sketch…"), "extrudeSketch");
    revolve_ = menuAction(model, tr("Revolve sketch…"), "revolveSketch");
    connect(extrude_, &QAction::triggered, this, &MainWindow::extrudeCurrent);
    connect(revolve_, &QAction::triggered, this, &MainWindow::revolveCurrent);
    booleanMenu_ = model->addMenu(tr("Combine bodies"));
    const auto booleanAction = [this](const QString& label, const char* name, geometry::BooleanOperation op) {
        auto* action = menuAction(booleanMenu_, label, name);
        connect(action, &QAction::triggered, this, [this, op] { combineCurrent(op); });
    };
    booleanAction(tr("Union…"), "booleanUnion", geometry::BooleanOperation::Union);
    booleanAction(tr("Cut…"), "booleanCut", geometry::BooleanOperation::Difference);
    booleanAction(tr("Intersection…"), "booleanIntersection", geometry::BooleanOperation::Intersection);

    viewMenu_ = menuBar()->addMenu(tr("&View"));
    auto* view = viewMenu_;
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

void MainWindow::createModelTree()
{
    auto* dock = new QDockWidget(tr("Model tree"), this);
    dock->setObjectName(QStringLiteral("modelDock"));
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    tree_->setObjectName(QStringLiteral("modelTree"));
    tree_->setHeaderLabels({tr("Object"), tr("Source / settings")});
    dock->setWidget(tree_);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    resizeDocks({dock}, {260}, Qt::Horizontal);
    connect(viewport_, &Viewport::sceneCleared, this, [this] {
        items_.clear();
        tree_->clear();
        updateActions();
    });
    connect(viewport_, &Viewport::objectAdded, this,
        [this](int id, const QString& name, bool sketch, const QString& detail, int parentId) {
            auto* parent = items_.value(parentId);
            auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
            const QSignalBlocker block(tree_);
            item->setText(0, name);
            item->setText(1, detail);
            item->setToolTip(1, detail);
            item->setData(0, objectIdRole, id);
            item->setData(0, sketchRole, sketch);
            item->setCheckState(0, Qt::Checked);
            items_.insert(id, item);
            if (parent) {
                parent->setExpanded(true);
            }
            updateActions();
        });
    connect(viewport_, &Viewport::objectSelected, this, [this](int id) {
        const QSignalBlocker block(tree_);
        tree_->setCurrentItem(items_.value(id));
        if (auto* item = tree_->currentItem()) {
            tree_->scrollToItem(item);
        }
        updateActions();
    });
    connect(viewport_, &Viewport::objectVisibilityChanged, this, [this](int id, bool visible) {
        if (auto* item = items_.value(id)) {
            const QSignalBlocker block(tree_);
            item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
        }
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        viewport_->selectObject(item ? item->data(0, objectIdRole).toInt() : 0);
        updateActions();
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (column != 0) {
            return;
        }
        const bool visible = item->checkState(0) == Qt::Checked;
        if (!viewport_->setObjectVisible(item->data(0, objectIdRole).toInt(), visible)) {
            const QSignalBlocker block(tree_);
            item->setCheckState(0, visible ? Qt::Unchecked : Qt::Checked);
        }
    });
}

void MainWindow::updateActions()
{
    if (!newScene_) {
        return;
    }
    const bool drawing = pages_->currentWidget() == sketchEditor_;
    const auto* item = tree_->currentItem();
    const bool sketch = item && item->data(0, sketchRole).toBool();
    const auto bodies = std::count_if(items_.begin(), items_.end(),
        [](const QTreeWidgetItem* row) { return !row->data(0, sketchRole).toBool(); });
    newScene_->setEnabled(!drawing);
    createMenu_->setEnabled(!drawing);
    editMenu_->setEnabled(!drawing);
    viewMenu_->setEnabled(!drawing);
    sketchMenu_->setEnabled(!drawing);
    tree_->setEnabled(!drawing);
    extrude_->setEnabled(!drawing && sketch);
    revolve_->setEnabled(!drawing && sketch);
    booleanMenu_->setEnabled(!drawing && item && !sketch && bodies >= 2);
}

void MainWindow::beginSketch(geometry::Plane plane)
{
    pages_->setCurrentWidget(sketchEditor_);
    sketchEditor_->begin(plane);
    statusBar()->showMessage(tr("Drawing sketch"));
    updateActions();
}

void MainWindow::finishSketch()
{
    const auto id = viewport_->addSketch(sketchEditor_->sketch());
    if (!id) {
        return;
    }
    leaveSketch();
    viewport_->fitScene();
    selectTreeObject(id);
}

void MainWindow::leaveSketch()
{
    pages_->setCurrentWidget(viewport_);
    viewport_->update();
    updateActions();
}

void MainWindow::selectTreeObject(int id)
{
    if (auto* item = items_.value(id)) {
        tree_->setCurrentItem(item);
        tree_->scrollToItem(item);
    }
}

void MainWindow::extrudeCurrent()
{
    auto* item = tree_->currentItem();
    if (!item || !item->data(0, sketchRole).toBool()) {
        return;
    }
    bool accepted = false;
    const auto distance = QInputDialog::getDouble(this, tr("Extrude sketch"),
        tr("Distance along the normal (mm). Negative values reverse direction."),
        30, -100000, 100000, 2, &accepted);
    if (accepted) {
        selectTreeObject(viewport_->extrudeSketch(item->data(0, objectIdRole).toInt(), distance));
    }
}

void MainWindow::revolveCurrent()
{
    auto* item = tree_->currentItem();
    if (!item || !item->data(0, sketchRole).toBool()) {
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("revolveDialog"));
    dialog.setWindowTitle(tr("Revolve sketch"));
    auto* form = new QFormLayout(&dialog);
    auto* axis = new QComboBox(&dialog);
    axis->setObjectName(QStringLiteral("revolveAxis"));
    axis->addItems({tr("U axis (horizontal)"), tr("V axis (vertical)")});
    form->addRow(tr("Axis"), axis);
    auto* offset = new QDoubleSpinBox(&dialog);
    offset->setObjectName(QStringLiteral("revolveOffset"));
    offset->setRange(-100000, 100000);
    offset->setSuffix(tr(" mm"));
    form->addRow(tr("Offset perpendicular to axis"), offset);
    auto* angle = new QDoubleSpinBox(&dialog);
    angle->setObjectName(QStringLiteral("revolveAngle"));
    angle->setRange(0.01, 360);
    angle->setValue(360);
    angle->setSuffix(QStringLiteral("°"));
    form->addRow(tr("Angle"), angle);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        const auto direction = axis->currentIndex() == 0 ? geometry::Axis::U : geometry::Axis::V;
        selectTreeObject(viewport_->revolveSketch(item->data(0, objectIdRole).toInt(),
                                                 direction, offset->value(), angle->value()));
    }
}

void MainWindow::combineCurrent(geometry::BooleanOperation operation)
{
    const auto* current = tree_->currentItem();
    if (!current || current->data(0, sketchRole).toBool()) {
        return;
    }
    auto ids = items_.keys();
    std::sort(ids.begin(), ids.end());
    QStringList names;
    QList<int> candidates;
    for (const int id : ids) {
        const auto* item = items_.value(id);
        if (item != current && !item->data(0, sketchRole).toBool()) {
            names.append(item->text(0));
            candidates.append(id);
        }
    }
    if (names.isEmpty()) {
        return;
    }
    bool accepted = false;
    const auto selected = QInputDialog::getItem(this, tr("Combine bodies"),
        tr("First body: %1. Select the second body. Cut removes it from the first.").arg(current->text(0)),
        names, 0, false, &accepted);
    if (accepted) {
        const auto index = names.indexOf(selected);
        if (index >= 0) {
            selectTreeObject(viewport_->combineBodies(current->data(0, objectIdRole).toInt(),
                                                      candidates.at(index), operation));
        }
    }
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
    if (pages_->currentWidget() == sketchEditor_) {
        sketchEditor_->showError(message);
    }
}

}  // namespace graph3d
