#include "gui/MainWindow.h"

#include "gui/PropertyPanel.h"
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
#include <QIcon>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTreeWidget>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <type_traits>

namespace graph3d {
namespace {

constexpr int objectIdRole = Qt::UserRole;


QAction* menuAction(QMenu* menu, const QString& label, const char* name)
{
    auto* action = menu->addAction(label);
    action->setObjectName(QString::fromLatin1(name));
    return action;
}

}  // namespace

MainWindow::MainWindow()
    : viewport_(new Viewport(this)), pages_(new QStackedWidget(this)),
      sketchEditor_(new SketchEditor(this)), tree_(new QTreeWidget(this)),
      properties_(new PropertyPanel(this))
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
    connect(properties_, &PropertyPanel::extrudeEdited, this, [this](int id, double distance) {
        editFeature(id, [this, id, distance] { return document_.editExtrude(id, distance); });
    });
    connect(properties_, &PropertyPanel::revolveEdited, this,
        [this](int id, geometry::Axis axis, double offset, double angle) {
            editFeature(id, [this, id, axis, offset, angle] {
                return document_.editRevolve(id, axis, offset, angle);
            });
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
    if (!addFeature(tr("Create cube"), [this] { return document_.addPrimitive(geometry::Primitive::Cube); }, false)) {
        return false;
    }
    document_.clearHistory();
    updateActions();
    statusBar()->showMessage(tr("Left click: select   |   Right drag: orbit   |   Middle drag: pan   |   Wheel: zoom"));
    return true;
}

void MainWindow::createMenus()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    newScene_ = menuAction(file, tr("&New scene"), "newScene");
    newScene_->setShortcut(QKeySequence::New);
    connect(newScene_, &QAction::triggered, this, [this] {
        if (document_.clear()) {
            refreshDocument(0);
        }
        viewport_->resetCamera();
        report(tr("New scene"));
    });
    file->addSeparator();
    auto* quit = file->addAction(tr("E&xit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    editMenu_ = menuBar()->addMenu(tr("&Edit"));
    undo_ = menuAction(editMenu_, tr("Undo"), "undoDocument");
    undo_->setShortcuts(QKeySequence::Undo);
    connect(undo_, &QAction::triggered, this, [this] { restoreHistory(true); });
    redo_ = menuAction(editMenu_, tr("Redo"), "redoDocument");
    redo_->setShortcuts(QKeySequence::Redo);
    connect(redo_, &QAction::triggered, this, [this] { restoreHistory(false); });
    editMenu_->addSeparator();
    auto* clear = menuAction(editMenu_, tr("Clear selection"), "clearSelection");
    clear->setShortcut(Qt::Key_Escape);
    connect(clear, &QAction::triggered, viewport_, &Viewport::clearSelection);

    createMenu_ = menuBar()->addMenu(tr("&Create"));
    const auto add = [this](const QString& label, const char* name, geometry::Primitive kind) {
        auto* action = menuAction(createMenu_, label, name);
        connect(action, &QAction::triggered, this, [this, kind] {
            addFeature(tr("Create primitive"), [this, kind] { return document_.addPrimitive(kind); }, false);
        });
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
    tree_->setHeaderLabels({tr("Object"), tr("Source / settings"), tr("State")});
    dock->setWidget(tree_);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    auto* propertyDock = new QDockWidget(tr("Properties"), this);
    propertyDock->setObjectName(QStringLiteral("propertiesDock"));
    propertyDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    propertyDock->setWidget(properties_);
    splitDockWidget(dock, propertyDock, Qt::Vertical);
    resizeDocks({dock}, {310}, Qt::Horizontal);

    connect(viewport_, &Viewport::objectSelected, this, [this](int id) {
        {
            const QSignalBlocker block(tree_);
            tree_->setCurrentItem(items_.value(id));
            if (auto* item = tree_->currentItem()) {
                tree_->scrollToItem(item);
            }
        }
        updateSelection();
        const auto* object = document_.find(id);
        report(object ? tr("Selected %1").arg(objectName(*object)) : tr("No selection"));
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*) {
        viewport_->selectObject(selectedId());
        updateSelection();
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (column != 0) {
            return;
        }
        const auto id = item->data(0, objectIdRole).toInt();
        const bool visible = item->checkState(0) == Qt::Checked;
        if (document_.find(id) && document_.setVisible(id, visible)) {
            refreshDocument(selectedId());
        }
    });
}

void MainWindow::refreshDocument(int selection)
{
    viewport_->syncDocument(document_);
    {
        const QSignalBlocker block(tree_);
        auto ids = items_.keys();
        std::sort(ids.begin(), ids.end(), std::greater<int>{});
        for (const int id : ids) {
            if (!document_.find(id)) {
                delete items_.take(id);
            }
        }
        for (const auto& object : document_.objects()) {
            auto* item = items_.value(object.id);
            if (!item) {
                int parentId = 0;
                if (const auto* extrusion = std::get_if<document::Extrude>(&object.parameters)) {
                    parentId = extrusion->profile;
                } else if (const auto* revolution = std::get_if<document::Revolve>(&object.parameters)) {
                    parentId = revolution->profile;
                }
                auto* parent = items_.value(parentId);
                item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
                items_.insert(object.id, item);
                item->setData(0, objectIdRole, object.id);
                if (parent) {
                    parent->setExpanded(true);
                }
            }
            const auto details = objectDetails(object);
            const auto error = QString::fromStdString(object.error);
            item->setText(0, objectName(object));
            item->setText(1, details);
            item->setToolTip(1, details);
            item->setText(2, object.status == document::Status::Failed ? tr("Invalid")
                           : object.status == document::Status::Blocked ? tr("Blocked") : QString{});
            item->setToolTip(2, error);
            item->setIcon(0, error.isEmpty() ? QIcon{} : style()->standardIcon(QStyle::SP_MessageBoxWarning));
            item->setCheckState(0, object.visible ? Qt::Checked : Qt::Unchecked);
        }
        tree_->setCurrentItem(items_.value(selection));
        if (auto* current = tree_->currentItem()) {
            tree_->scrollToItem(current);
        }
    }
    viewport_->selectObject(selectedId());
    updateSelection();
}

int MainWindow::selectedId() const
{
    const auto* item = tree_->currentItem();
    return item ? item->data(0, objectIdRole).toInt() : 0;
}

void MainWindow::updateSelection()
{
    const auto* object = document_.find(selectedId());
    properties_->showObject(object, object ? objectName(*object) : QString{},
                            object ? objectDetails(*object) : QString{});
    updateActions();
}

void MainWindow::updateActions()
{
    if (!newScene_) {
        return;
    }
    const bool drawing = pages_->currentWidget() == sketchEditor_;
    const auto* object = document_.find(selectedId());
    const bool sketch = object && std::holds_alternative<geometry::Sketch>(object->parameters);
    const auto validBody = [](const document::Object& candidate) {
        return candidate.status == document::Status::Ready
            && !std::holds_alternative<geometry::Sketch>(candidate.parameters);
    };
    const auto bodies = std::count_if(document_.objects().begin(), document_.objects().end(), validBody);
    newScene_->setEnabled(!drawing);
    undo_->setEnabled(!drawing && document_.canUndo());
    redo_->setEnabled(!drawing && document_.canRedo());
    createMenu_->setEnabled(!drawing);
    editMenu_->setEnabled(!drawing);
    viewMenu_->setEnabled(!drawing);
    sketchMenu_->setEnabled(!drawing);
    tree_->setEnabled(!drawing);
    properties_->setEnabled(!drawing);
    extrude_->setEnabled(!drawing && sketch);
    revolve_->setEnabled(!drawing && sketch);
    booleanMenu_->setEnabled(!drawing && object && validBody(*object) && bodies >= 2);
}

QString MainWindow::objectName(const document::Object& object) const
{
    const auto name = std::visit([this](const auto& operation) -> QString {
        using Type = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Type, document::Primitive>) {
            switch (operation.kind) {
            case geometry::Primitive::Cube: return tr("Cube");
            case geometry::Primitive::Cylinder: return tr("Cylinder");
            case geometry::Primitive::Sphere: return tr("Sphere");
            }
        } else if constexpr (std::is_same_v<Type, geometry::Sketch>) {
            return tr("Sketch");
        } else if constexpr (std::is_same_v<Type, document::Extrude>) {
            return tr("Extrude");
        } else if constexpr (std::is_same_v<Type, document::Revolve>) {
            return tr("Revolve");
        } else {
            switch (operation.operation) {
            case geometry::BooleanOperation::Union: return tr("Union");
            case geometry::BooleanOperation::Difference: return tr("Cut");
            case geometry::BooleanOperation::Intersection: return tr("Intersection");
            }
        }
        return tr("Object");
    }, object.parameters);
    return tr("%1 %2").arg(name).arg(object.id);
}

QString MainWindow::objectDetails(const document::Object& object) const
{
    return std::visit([this](const auto& operation) -> QString {
        using Type = std::decay_t<decltype(operation)>;
        const auto source = [this](int id) {
            const auto* input = document_.find(id);
            return input ? objectName(*input) : QString::number(id);
        };
        if constexpr (std::is_same_v<Type, geometry::Sketch>) {
            switch (operation.plane) {
            case geometry::Plane::XY: return tr("Plane XY");
            case geometry::Plane::XZ: return tr("Plane XZ");
            case geometry::Plane::YZ: return tr("Plane YZ");
            }
        } else if constexpr (std::is_same_v<Type, document::Extrude>) {
            return tr("%1 · %2 mm").arg(source(operation.profile)).arg(operation.distance);
        } else if constexpr (std::is_same_v<Type, document::Revolve>) {
            return tr("%1 · %2° · %3 axis, offset %4 mm").arg(source(operation.profile)).arg(operation.angle)
                .arg(operation.axis == geometry::Axis::U ? "U" : "V").arg(operation.offset);
        } else if constexpr (std::is_same_v<Type, document::Boolean>) {
            return tr("%1 and %2").arg(source(operation.first), source(operation.second));
        }
        return {};
    }, object.parameters);
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
    const auto id = addFeature(tr("Finish sketch"), [this] {
        return document_.addSketch(sketchEditor_->sketch());
    });
    if (id) {
        leaveSketch();
        viewport_->fitScene();
    }
}

void MainWindow::leaveSketch()
{
    pages_->setCurrentWidget(viewport_);
    viewport_->update();
    updateActions();
}

int MainWindow::addFeature(const QString& operation, const std::function<int()>& action, bool select)
{
    try {
        const auto id = action();
        refreshDocument(select ? id : selectedId());
        if (pages_->currentWidget() == viewport_) {
            viewport_->fitScene();
        }
        report(tr("Created %1").arg(objectName(*document_.find(id))));
        return id;
    } catch (const std::exception& error) {
        showSceneError(tr("%1: %2").arg(operation, QString::fromUtf8(error.what())));
        return 0;
    }
}

void MainWindow::editFeature(int id, const std::function<bool()>& action)
{
    try {
        if (!action()) {
            return;
        }
        refreshDocument(id);
        const auto& object = *document_.find(id);
        if (object.status != document::Status::Ready) {
            showSceneError(tr("%1: %2").arg(objectName(object), QString::fromStdString(object.error)));
            return;
        }
        const auto waiting = std::count_if(document_.objects().begin(), document_.objects().end(),
            [](const auto& candidate) { return candidate.status != document::Status::Ready; });
        report(waiting ? tr("Rebuilt %1. %2 operations need attention in the model tree.")
                         .arg(objectName(object)).arg(waiting)
                       : tr("Rebuilt %1 and its dependent operations.").arg(objectName(object)));
    } catch (const std::exception& error) {
        showSceneError(QString::fromUtf8(error.what()));
    }
}

void MainWindow::restoreHistory(bool undo)
{
    const auto selection = selectedId();
    if (undo ? document_.undo() : document_.redo()) {
        refreshDocument(selection);
        report(undo ? tr("Undone") : tr("Redone"));
    }
}

void MainWindow::extrudeCurrent()
{
    const auto id = selectedId();
    const auto* object = document_.find(id);
    if (!object || !std::holds_alternative<geometry::Sketch>(object->parameters)) {
        return;
    }
    bool accepted = false;
    const auto distance = QInputDialog::getDouble(this, tr("Extrude sketch"),
        tr("Distance along the normal (mm). Negative values reverse direction."),
        30, -100000, 100000, 2, &accepted);
    if (accepted) {
        addFeature(tr("Extrude"), [this, id, distance] { return document_.addExtrude(id, distance); });
    }
}

void MainWindow::revolveCurrent()
{
    const auto id = selectedId();
    const auto* object = document_.find(id);
    if (!object || !std::holds_alternative<geometry::Sketch>(object->parameters)) {
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
        addFeature(tr("Revolve"), [this, id, direction, offset, angle] {
            return document_.addRevolve(id, direction, offset->value(), angle->value());
        });
    }
}

void MainWindow::combineCurrent(geometry::BooleanOperation operation)
{
    const auto first = selectedId();
    const auto* current = document_.find(first);
    if (!current || current->status != document::Status::Ready
        || std::holds_alternative<geometry::Sketch>(current->parameters)) {
        return;
    }
    QStringList names;
    QList<int> candidates;
    for (const auto& object : document_.objects()) {
        if (object.id != first && object.status == document::Status::Ready
            && !std::holds_alternative<geometry::Sketch>(object.parameters)) {
            names.append(objectName(object));
            candidates.append(object.id);
        }
    }
    if (names.isEmpty()) {
        return;
    }
    bool accepted = false;
    const auto selected = QInputDialog::getItem(this, tr("Combine bodies"),
        tr("First body: %1. Select the second body. Cut removes it from the first.").arg(objectName(*current)),
        names, 0, false, &accepted);
    if (accepted) {
        const auto index = names.indexOf(selected);
        if (index >= 0) {
            const auto second = candidates.at(index);
            addFeature(tr("Combine bodies"), [this, first, second, operation] {
                return document_.addBoolean(first, second, operation);
            });
        }
    }
}

void MainWindow::report(const QString& message)
{
    statusBar()->showMessage(message);
    writeLog(message);
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
