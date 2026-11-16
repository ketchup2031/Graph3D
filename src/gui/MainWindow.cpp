#include "gui/MainWindow.h"

#include "gui/AdvancedDialog.h"
#include "gui/AssemblyDialog.h"
#include "gui/DraftWindow.h"
#include "gui/DrawingDialog.h"
#include "gui/InterfaceLanguage.h"
#include "gui/PropertyPanel.h"
#include "gui/SketchEditor.h"
#include "gui/StartPage.h"
#include "gui/TransformEditor.h"
#include "gui/UserPreferences.h"
#include "io/Exchange.h"
#include "io/ProjectIO.h"
#include "viewport/Viewport.h"

#include <Standard_Failure.hxx>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
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
#include <QFileDialog>
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
#include <cmath>
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
      sketchEditor_(new SketchEditor(this)), startPage_(new StartPage(this)), tree_(new QTreeWidget(this)),
      properties_(new PropertyPanel(this))
{
    setWindowTitle(QStringLiteral("Graph3D[*]"));
    resize(1040, 710);
    setMinimumSize(480, 320);
    setCentralWidget(pages_);
    pages_->addWidget(startPage_);
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
    connect(startPage_, &StartPage::newDrawingRequested, this, [this] { showDraft(); });
    connect(startPage_, &StartPage::newProjectRequested, this, &MainWindow::newProject);
    connect(startPage_, &StartPage::cubeRequested, this, &MainWindow::initializeScene);
    connect(startPage_, &StartPage::sketchRequested, this, [this] {
        if (newProject()) {
            beginSketch(geometry::Plane::XY);
        }
    });
    connect(startPage_, &StartPage::openRequested, this, &MainWindow::openProject);
    connect(startPage_, &StartPage::recentRequested, this, [this](const QString& path) { openProjectPath(path); });
    connect(startPage_, &StartPage::exampleRequested, this, [this](const QString& path) { openProjectPath(path, true); });
    connect(startPage_, &StartPage::resumeRequested, this, &MainWindow::showWorkspace);
    connect(startPage_, &StartPage::controlsRequested, this, &MainWindow::showMouseControls);
    showStartPage();
    connect(properties_, &PropertyPanel::extrudeEdited, this, [this](int id, double distance) {
        editFeature(id, [this, id, distance] { return document_.editExtrude(id, distance); });
    });
    connect(properties_, &PropertyPanel::revolveEdited, this,
        [this](int id, geometry::Axis axis, double offset, double angle) {
            editFeature(id, [this, id, axis, offset, angle] {
                return document_.editRevolve(id, axis, offset, angle);
            });
        });
    connect(properties_, &PropertyPanel::holeEdited, this, [this](int id, double diameter, double depth, bool through) {
        editFeature(id, [this, id, diameter, depth, through] {
            return document_.editHole(id, {diameter, depth, through});
        });
    });
    connect(properties_, &PropertyPanel::edgeTreatmentEdited, this, [this](int id, double size) {
        editFeature(id, [this, id, size] { return document_.editEdgeTreatment(id, size); });
    });
    connect(properties_, &PropertyPanel::transformEdited, this,
        [this](int id, const geometry::BodyTransform& parameters) {
            editFeature(id, [this, id, parameters] { return document_.editTransform(id, parameters); });
        });
    connect(properties_, &PropertyPanel::assemblyEditRequested, this, &MainWindow::configureAssembly);
    connect(viewport_, &Viewport::operationFailed, this, &MainWindow::showSceneError);
    connect(viewport_, &Viewport::facePicked, this, &MainWindow::makeHole);
    connect(viewport_, &Viewport::edgePicked, this, &MainWindow::makeEdgeTreatment);
    connect(viewport_, &Viewport::vertexPicked, this, [this](int, const geometry::Point3& point) {
        if (pendingMachining_ != Machining::MeasureVertices) {
            return;
        }
        if (!pickedVertex_) {
            pickedVertex_ = point;
            statusBar()->showMessage(tr("Pick the second vertex on the selected body."));
            return;
        }
        const auto a = *pickedVertex_;
        const auto distance = std::hypot(a[0] - point[0], a[1] - point[1], a[2] - point[2]);
        viewport_->cancelSubshapePick();
        report(tr("Vertex distance: %1 mm").arg(distance, 0, 'g', 10));
    });
    connect(viewport_, &Viewport::subshapePickCancelled, this, [this] {
        endMachining(selectedId());
        report(tr("Geometry selection cancelled"));
    });
}

bool MainWindow::initializeScene()
{
    if (!newProject()) {
        return false;
    }
    if (!addFeature(tr("Create cube"), [this] { return document_.addPrimitive(geometry::Primitive::Cube); }, false)) {
        return false;
    }
    document_.clearHistory();
    savedRevision_ = document_.revision();
    updateFileState();
    updateActions();
    statusBar()->showMessage(tr("Left click: select   |   Right drag: orbit   |   Middle drag: pan   |   Wheel: zoom"));
    return true;
}

void MainWindow::showStartPage()
{
    viewport_->cancelSubshapePick();
    pages_->setCurrentWidget(startPage_);
    modelDock_->hide();
    propertyDock_->hide();
    startPage_->setWorkspaceAvailable(hasWorkspace_);
    statusBar()->clearMessage();
    updateActions();
}

bool MainWindow::showWorkspace()
{
    pages_->setCurrentWidget(viewport_);
    modelDock_->show();
    propertyDock_->show();
    if (!viewport_->initialize()) {
        const auto error = statusBar()->currentMessage();
        showStartPage();
        statusBar()->showMessage(error);
        return false;
    }
    if (!hasWorkspace_) {
        writeLog(tr("3D viewport initialized"));
    }
    hasWorkspace_ = true;
    updateActions();
    return true;
}

bool MainWindow::newProject()
{
    if (!confirmSave() || !showWorkspace()) {
        return false;
    }
    document_ = document::Document{};
    projectPath_.clear();
    savedRevision_ = document_.revision();
    refreshDocument(0);
    viewport_->resetCamera();
    report(tr("New part"));
    return true;
}

void MainWindow::showMouseControls()
{
    QMessageBox::information(this, tr("Mouse controls"),
        tr("Left click: select a body\nRight drag: orbit\nMiddle drag: pan\nWheel: zoom\nF: fit all"));
}

void MainWindow::createMenus()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    startPageAction_ = menuAction(file, tr("Start page"), "showStartPage");
    connect(startPageAction_, &QAction::triggered, this, &MainWindow::showStartPage);
    file->addSeparator();
    newScene_ = menuAction(file, tr("&New scene"), "newScene");
    newScene_->setShortcut(QKeySequence::New);
    connect(newScene_, &QAction::triggered, this, [this] {
        if (pages_->currentWidget() == startPage_) {
            newProject();
            return;
        }
        if (document_.clear()) {
            refreshDocument(0);
        }
        viewport_->resetCamera();
        report(tr("New scene"));
    });
    openAction_ = menuAction(file, tr("&Open project…"), "openProject");
    openAction_->setShortcut(QKeySequence::Open);
    connect(openAction_, &QAction::triggered, this, &MainWindow::openProject);
    auto* save = menuAction(file, tr("&Save project"), "saveProject");
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, [this] { saveProject(false); });
    auto* saveAs = menuAction(file, tr("Save project &as…"), "saveProjectAs");
    saveAs->setShortcut(QKeySequence::SaveAs);
    connect(saveAs, &QAction::triggered, this, [this] { saveProject(true); });

    file->addSeparator();
    auto* import = menuAction(file, tr("Import STEP…"), "importStep");
    connect(import, &QAction::triggered, this, &MainWindow::importStep);
    auto* exportStep = menuAction(file, tr("Export visible bodies to STEP…"), "exportStep");
    connect(exportStep, &QAction::triggered, this, [this] { exportModel(io::ExchangeFormat::Step); });
    auto* exportStl = menuAction(file, tr("Export visible bodies to STL…"), "exportStl");
    connect(exportStl, &QAction::triggered, this, [this] { exportModel(io::ExchangeFormat::Stl); });
    fileActions_ = {save, saveAs, import, exportStep, exportStl};

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
    createMenu_->addSeparator();
    auto* plane = menuAction(createMenu_, tr("Construction plane…"), "makeDatumPlane");
    auto* axis = menuAction(createMenu_, tr("Construction axis…"), "makeDatumAxis");
    connect(plane, &QAction::triggered, this, [this] { configureAdvanced(tr("Construction plane"), geometry::DatumPlane{}); });
    connect(axis, &QAction::triggered, this, [this] { configureAdvanced(tr("Construction axis"), geometry::DatumAxis{}); });

    sketchMenu_ = menuBar()->addMenu(tr("&Sketch"));
    const auto planeAction = [this](const QString& label, const char* name, geometry::Plane plane) {
        auto* action = menuAction(sketchMenu_, label, name);
        connect(action, &QAction::triggered, this, [this, plane] { beginSketch(plane); });
    };
    planeAction(tr("New on XY"), "newSketchXY", geometry::Plane::XY);
    planeAction(tr("New on XZ"), "newSketchXZ", geometry::Plane::XZ);
    planeAction(tr("New on YZ"), "newSketchYZ", geometry::Plane::YZ);
    newFaceSketch_ = menuAction(sketchMenu_, tr("New on planar face"), "newSketchOnFace");
    connect(newFaceSketch_, &QAction::triggered, this, [this] { startMachining(Machining::SketchFace); });
    newDatumSketch_ = menuAction(sketchMenu_, tr("New on construction plane"), "newSketchOnDatum");
    connect(newDatumSketch_, &QAction::triggered, this, [this] {
        geometry::Sketch sketch;
        sketch.datumPlane = selectedId();
        beginSketch(sketch);
    });
    editSketch_ = menuAction(sketchMenu_, tr("Edit sketch"), "editSketch");
    connect(editSketch_, &QAction::triggered, this, [this] { editSketch(selectedId()); });

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

    model->addSeparator();
    const auto machiningAction = [this, model](const QString& text, const char* name, Machining operation) {
        auto* action = menuAction(model, text, name);
        connect(action, &QAction::triggered, this, [this, operation] { startMachining(operation); });
        machiningActions_.append(action);
    };
    machiningAction(tr("Hole…"), "makeHole", Machining::Hole);
    machiningAction(tr("Chamfer…"), "makeChamfer", Machining::Chamfer);
    machiningAction(tr("Fillet…"), "makeFillet", Machining::Fillet);
    machiningAction(tr("Chamfer multiple edges…"), "makeMultiChamfer", Machining::ChamferSet);
    machiningAction(tr("Fillet multiple edges…"), "makeMultiFillet", Machining::FilletSet);
    finishEdgeSet_ = menuAction(model, tr("Finish selecting edges"), "finishEdgeSet");
    connect(finishEdgeSet_, &QAction::triggered, this, &MainWindow::finishEdgeSet);
    connect(viewport_, &Viewport::edgeSetFinished, this, &MainWindow::finishEdgeSet);
    model->addSeparator();
    sweep_ = menuAction(model, tr("Sweep along path…"), "makeSweep");
    loft_ = menuAction(model, tr("Loft through sections…"), "makeLoft");
    shell_ = menuAction(model, tr("Shell…"), "makeShell");
    draftAngle_ = menuAction(model, tr("Draft angle…"), "makeDraftAngle");
    editAdvanced_ = menuAction(model, tr("Edit modeling operation…"), "editAdvancedFeature");
    connect(sweep_, &QAction::triggered, this, [this] {
        configureAdvanced(tr("Sweep"), document::Sweep{selectedId(), {{0, 0, 0}, {0, 0, 35}, {40, 0, 35}}});
    });
    connect(loft_, &QAction::triggered, this, [this] {
        const auto* object = document_.find(selectedId());
        std::vector<int> first;
        if (object && object->status == document::Status::Ready && std::holds_alternative<geometry::Sketch>(object->parameters)) {
            first.push_back(object->id);
        }
        configureAdvanced(tr("Loft"), document::Loft{std::move(first), false});
    });
    connect(shell_, &QAction::triggered, this, [this] {
        configureAdvanced(tr("Shell"), document::Shell{selectedId(), {}, 3});
    });
    connect(draftAngle_, &QAction::triggered, this, [this] {
        configureAdvanced(tr("Draft angle"), document::DraftAngle{selectedId(), {}, {}, 5});
    });
    connect(editAdvanced_, &QAction::triggered, this, [this] {
        const auto* object = document_.find(selectedId());
        if (object && document::isAdvanced(object->parameters)) {
            configureAdvanced(objectName(*object), object->parameters, object->id);
        }
    });
    auto* measure = model->addMenu(tr("Measure"));
    measureBody_ = menuAction(measure, tr("Volume and mass…"), "measureBody");
    measureDistance_ = menuAction(measure, tr("Distance between bodies…"), "measureBodyDistance");
    auto* vertices = menuAction(measure, tr("Distance between vertices"), "measureVertices");
    machiningActions_.append(vertices);
    connect(measureBody_, &QAction::triggered, this, &MainWindow::measureBody);
    connect(measureDistance_, &QAction::triggered, this, &MainWindow::measureBodyDistance);
    connect(vertices, &QAction::triggered, this, [this] { startMachining(Machining::MeasureVertices); });

    transformMenu_ = model->addMenu(tr("Transform body"));
    const auto transformAction = [this](const QString& title, const char* name, geometry::BodyTransform defaults) {
        auto* action = menuAction(transformMenu_, title + QStringLiteral("…"), name);
        connect(action, &QAction::triggered, this, [this, title, defaults] {
            transformCurrent(title, defaults);
        });
    };
    transformAction(tr("Linear array"), "makeLinearArray", geometry::LinearArray{geometry::ModelAxis::X, 3, 100});
    transformAction(tr("Circular array"), "makePolarArray", geometry::PolarArray{geometry::ModelAxis::Z, 4, 360});
    transformAction(tr("Mirror"), "makeMirror", geometry::Mirror{geometry::Plane::YZ, 0});

    createAssembly_ = menuAction(model, tr("Create assembly…"), "createAssembly");
    connect(createAssembly_, &QAction::triggered, this, [this] { configureAssembly(0); });

    auto* drawingMenu = menuBar()->addMenu(tr("&Drawing"));
    auto* newDraft = menuAction(drawingMenu, tr("New 2D drawing…"), "newDrawing");
    connect(newDraft, &QAction::triggered, this, [this] { showDraft(); });
    drawing_ = menuAction(drawingMenu, tr("Create drawing from selected body…"), "drawingPreview");
    connect(drawing_, &QAction::triggered, this, &MainWindow::showDrawing);

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

    auto* settings = menuBar()->addMenu(tr("Sett&ings"));
    auto* theme = menuAction(settings, tr("Dark theme"), "darkTheme");
    theme->setCheckable(true);
    theme->setChecked(darkThemeEnabled());
    applyTheme(theme->isChecked());
    startPage_->setDarkTheme(theme->isChecked());
    connect(theme, &QAction::toggled, this, [this](bool dark) {
        userSettings().setValue(QStringLiteral("appearance/darkTheme"), dark);
        applyTheme(dark);
        startPage_->setDarkTheme(dark);
    });

    auto* languageMenu = settings->addMenu(tr("Language"));
    auto* languages = new QActionGroup(languageMenu);
    const auto languageChoice = [this, languageMenu, languages](const QString& label, const char* name, bool russian) {
        auto* action = menuAction(languageMenu, label, name);
        action->setCheckable(true);
        languages->addAction(action);
        action->setChecked(russian == russianLanguagePreferred());
        connect(action, &QAction::triggered, this, [this, russian] {
            if (russian == russianLanguagePreferred()) {
                return;
            }
            setRussianLanguagePreferred(russian);
            if (russian != russianInterface()) {
                QMessageBox::information(this, tr("Language settings"),
                    tr("Restart Graph3D to apply the language change."));
            }
        });
    };
    languageChoice(QStringLiteral("English"), "languageEnglish", false);
    languageChoice(QStringLiteral("Русский"), "languageRussian", true);

    auto* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Mouse controls"), this, &MainWindow::showMouseControls);
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
    auto* dock = modelDock_ = new QDockWidget(tr("Model tree"), this);
    dock->setObjectName(QStringLiteral("modelDock"));
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    tree_->setObjectName(QStringLiteral("modelTree"));
    tree_->setHeaderLabels({tr("Object"), tr("Source / settings"), tr("State")});
    dock->setWidget(tree_);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    auto* propertyDock = propertyDock_ = new QDockWidget(tr("Properties"), this);
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
        viewport_->cancelSubshapePick();
        viewport_->selectObject(selectedId());
        updateSelection();
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        const auto id = item->data(0, objectIdRole).toInt();
        const auto* object = document_.find(id);
        if (object && document::isAdvanced(object->parameters)) {
            configureAdvanced(objectName(*object), object->parameters, id);
        } else {
            editSketch(id);
        }
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
                if (const auto* sketch = std::get_if<geometry::Sketch>(&object.parameters); sketch && sketch->support) {
                    parentId = sketch->support->body;
                } else if (const auto* sketch = std::get_if<geometry::Sketch>(&object.parameters); sketch && sketch->datumPlane) {
                    parentId = sketch->datumPlane;
                } else if (const auto* sweep = std::get_if<document::Sweep>(&object.parameters)) {
                    parentId = sweep->profile;
                } else if (const auto* shell = std::get_if<document::Shell>(&object.parameters)) {
                    parentId = shell->body;
                } else if (const auto* draft = std::get_if<document::DraftAngle>(&object.parameters)) {
                    parentId = draft->body;
                } else if (const auto* extrusion = std::get_if<document::Extrude>(&object.parameters)) {
                    parentId = extrusion->profile;
                } else if (const auto* revolution = std::get_if<document::Revolve>(&object.parameters)) {
                    parentId = revolution->profile;
                } else if (const auto* hole = std::get_if<document::Hole>(&object.parameters)) {
                    parentId = hole->body;
                } else if (const auto* edge = std::get_if<document::EdgeTreatment>(&object.parameters)) {
                    parentId = edge->body;
                } else if (const auto* transform = std::get_if<document::Transform>(&object.parameters)) {
                    parentId = transform->body;
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
            const auto error = localizedDiagnostic(object.error.c_str());
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
    updateFileState();
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
    const bool modeling = pages_->currentWidget() == viewport_;
    const auto* object = document_.find(selectedId());
    const bool sketch = object && std::holds_alternative<geometry::Sketch>(object->parameters);
    const auto validBody = [](const document::Object& candidate) {
        return candidate.status == document::Status::Ready
            && document::isBody(candidate.parameters);
    };
    const auto bodies = std::count_if(document_.objects().begin(), document_.objects().end(), validBody);
    const auto sketches = std::ranges::count_if(document_.objects(), [](const auto& item) {
        return item.status == document::Status::Ready && std::holds_alternative<geometry::Sketch>(item.parameters);
    });
    startPageAction_->setEnabled(modeling);
    openAction_->setEnabled(!drawing);
    newScene_->setEnabled(!drawing);
    for (auto* action : fileActions_) {
        action->setEnabled(modeling);
    }
    undo_->setEnabled(modeling && document_.canUndo());
    redo_->setEnabled(modeling && document_.canRedo());
    createMenu_->setEnabled(modeling);
    editMenu_->setEnabled(modeling);
    viewMenu_->setEnabled(modeling);
    sketchMenu_->setEnabled(modeling);
    tree_->setEnabled(modeling);
    properties_->setEnabled(modeling);
    editSketch_->setEnabled(modeling && sketch && !pendingMachining_);
    const bool available = modeling && !pendingMachining_;
    const bool selectedBody = object && validBody(*object);
    newDatumSketch_->setEnabled(available && object && object->status == document::Status::Ready
        && std::holds_alternative<geometry::DatumPlane>(object->parameters));
    sweep_->setEnabled(available && sketch && object->status == document::Status::Ready);
    loft_->setEnabled(available && sketches >= 2);
    shell_->setEnabled(available && selectedBody);
    draftAngle_->setEnabled(available && selectedBody);
    editAdvanced_->setEnabled(available && object && document::isAdvanced(object->parameters));
    measureBody_->setEnabled(available && selectedBody);
    measureDistance_->setEnabled(available && selectedBody && bodies >= 2);
    finishEdgeSet_->setEnabled(modeling && !pickedEdges_.empty()
        && (pendingMachining_ == Machining::ChamferSet || pendingMachining_ == Machining::FilletSet));
    newFaceSketch_->setEnabled(modeling && !pendingMachining_ && object && object->visible && validBody(*object));
    extrude_->setEnabled(modeling && sketch && object->status == document::Status::Ready);
    revolve_->setEnabled(modeling && sketch && object->status == document::Status::Ready);
    booleanMenu_->setEnabled(modeling && object && validBody(*object) && bodies >= 2);
    transformMenu_->setEnabled(modeling && !pendingMachining_ && object && validBody(*object));
    createAssembly_->setEnabled(modeling && !pendingMachining_ && bodies > 0);
    drawing_->setEnabled(modeling && object && validBody(*object));
    for (auto* action : machiningActions_) {
        action->setEnabled(modeling && !pendingMachining_ && object && object->visible && validBody(*object));
    }
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
        } else if constexpr (std::is_same_v<Type, geometry::DatumPlane>) {
            return tr("Construction plane");
        } else if constexpr (std::is_same_v<Type, geometry::DatumAxis>) {
            return tr("Construction axis");
        } else if constexpr (std::is_same_v<Type, document::Sweep>) {
            return tr("Sweep");
        } else if constexpr (std::is_same_v<Type, document::Loft>) {
            return tr("Loft");
        } else if constexpr (std::is_same_v<Type, document::Shell>) {
            return tr("Shell");
        } else if constexpr (std::is_same_v<Type, document::DraftAngle>) {
            return tr("Draft angle");
        } else if constexpr (std::is_same_v<Type, document::Extrude>) {
            return tr("Extrude");
        } else if constexpr (std::is_same_v<Type, document::Revolve>) {
            return tr("Revolve");
        } else if constexpr (std::is_same_v<Type, document::Imported>) {
            return tr("Imported STEP");
        } else if constexpr (std::is_same_v<Type, document::Hole>) {
            return tr("Hole");
        } else if constexpr (std::is_same_v<Type, document::EdgeTreatment>) {
            return operation.kind == geometry::EdgeTreatmentKind::Chamfer ? tr("Chamfer") : tr("Fillet");
        } else if constexpr (std::is_same_v<Type, document::Transform>) {
            if (std::holds_alternative<geometry::LinearArray>(operation.parameters)) {
                return tr("Linear array");
            }
            if (std::holds_alternative<geometry::PolarArray>(operation.parameters)) {
                return tr("Circular array");
            }
            return tr("Mirror");
        } else if constexpr (std::is_same_v<Type, document::Assembly>) {
            return tr("Assembly");
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
            if (operation.support) {
                return tr("Planar face · %1").arg(source(operation.support->body));
            }
            if (operation.datumPlane) {
                return source(operation.datumPlane);
            }
            switch (operation.plane) {
            case geometry::Plane::XY: return tr("Plane XY");
            case geometry::Plane::XZ: return tr("Plane XZ");
            case geometry::Plane::YZ: return tr("Plane YZ");
            }
        } else if constexpr (std::is_same_v<Type, document::Extrude>) {
            return tr("%1 · %2 mm").arg(source(operation.profile)).arg(operation.distance);
        } else if constexpr (std::is_same_v<Type, document::Revolve>) {
            if (operation.datumAxis) {
                return tr("%1 · %2° · %3").arg(source(operation.profile)).arg(operation.angle).arg(source(operation.datumAxis));
            }
            return tr("%1 · %2° · %3 axis, offset %4 mm").arg(source(operation.profile)).arg(operation.angle)
                .arg(operation.axis == geometry::Axis::U ? "U" : "V").arg(operation.offset);
        } else if constexpr (std::is_same_v<Type, document::Boolean>) {
            return tr("%1 and %2").arg(source(operation.first), source(operation.second));
        } else if constexpr (std::is_same_v<Type, document::Imported>) {
            return QString::fromStdString(operation.name);
        } else if constexpr (std::is_same_v<Type, document::Hole>) {
            const auto mode = operation.size.through ? tr("Through")
                : tr("Depth %1 mm").arg(operation.size.depth);
            return tr("%1 · ⌀%2 mm · %3").arg(source(operation.body)).arg(operation.size.diameter).arg(mode);
        } else if constexpr (std::is_same_v<Type, document::EdgeTreatment>) {
            const auto size = operation.kind == geometry::EdgeTreatmentKind::Chamfer
                ? tr("Distance %1 mm").arg(operation.size) : tr("Radius %1 mm").arg(operation.size);
            return tr("%1 · %2 · %3 edges").arg(source(operation.body), size).arg(operation.additionalEdges.size() + 1);
        } else if constexpr (std::is_same_v<Type, document::Sweep>) {
            return tr("%1 · %2 path points").arg(source(operation.profile)).arg(operation.path.size());
        } else if constexpr (std::is_same_v<Type, document::Loft>) {
            return tr("Sections: %1").arg(operation.profiles.size());
        } else if constexpr (std::is_same_v<Type, document::Shell>) {
            return tr("%1 · %2 mm wall").arg(source(operation.body)).arg(operation.thickness);
        } else if constexpr (std::is_same_v<Type, document::DraftAngle>) {
            return tr("%1 · %2°").arg(source(operation.body)).arg(operation.degrees);
        } else if constexpr (std::is_same_v<Type, document::Transform>) {
            return source(operation.body);
        } else if constexpr (std::is_same_v<Type, document::Assembly>) {
            return tr("Components: %1").arg(operation.components.size());
        }
        return {};
    }, object.parameters);
}

void MainWindow::beginSketch(geometry::Plane plane)
{
    beginSketch(geometry::Sketch{plane});
}

void MainWindow::beginSketch(const geometry::Sketch& sketch, int id)
{
    try {
        auto context = document_.sketchContext(sketch);
        viewport_->cancelSubshapePick();
        editingSketch_ = id;
        pages_->setCurrentWidget(sketchEditor_);
        sketchEditor_->begin(sketch, std::move(context.projections));
    } catch (const std::exception& error) {
        showSceneError(localizedDiagnostic(error.what()));
        return;
    } catch (const Standard_Failure& error) {
        showSceneError(localizedDiagnostic(error.GetMessageString()));
        return;
    }
    statusBar()->showMessage(tr("Drawing sketch"));
    updateActions();
}

void MainWindow::editSketch(int id)
{
    const auto* object = document_.find(id);
    if (pages_->currentWidget() == viewport_ && object) {
        if (const auto* sketch = std::get_if<geometry::Sketch>(&object->parameters)) {
            beginSketch(*sketch, id);
        }
    }
}

void MainWindow::finishSketch()
{
    if (editingSketch_ != 0) {
        const int id = editingSketch_;
        try {
            const bool modified = document_.editSketch(id, sketchEditor_->sketch());
            leaveSketch();
            refreshDocument(id);
            if (modified) {
                const auto problems = std::ranges::count_if(document_.objects(),
                    [](const auto& object) { return object.status != document::Status::Ready; });
                report(problems ? tr("Rebuilt %1. %2 operations need attention in the model tree.")
                    .arg(objectName(*document_.find(id))).arg(problems)
                    : tr("Rebuilt %1 and its dependent operations.").arg(objectName(*document_.find(id))));
            }
        } catch (const std::exception& error) {
            showSceneError(localizedDiagnostic(error.what()));
        }
        return;
    }
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
    editingSketch_ = 0;
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
        showSceneError(tr("%1: %2").arg(operation, localizedDiagnostic(error.what())));
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
            showSceneError(tr("%1: %2").arg(objectName(object), localizedDiagnostic(object.error.c_str())));
            return;
        }
        const auto waiting = std::count_if(document_.objects().begin(), document_.objects().end(),
            [](const auto& candidate) { return candidate.status != document::Status::Ready; });
        report(waiting ? tr("Rebuilt %1. %2 operations need attention in the model tree.")
                         .arg(objectName(object)).arg(waiting)
                       : tr("Rebuilt %1 and its dependent operations.").arg(objectName(object)));
    } catch (const std::exception& error) {
        showSceneError(localizedDiagnostic(error.what()));
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
    axis->addItem(tr("U axis (horizontal)"), 0);
    axis->addItem(tr("V axis (vertical)"), 0);
    for (const auto& item : document_.objects()) {
        if (item.status == document::Status::Ready && std::holds_alternative<geometry::DatumAxis>(item.parameters)) {
            axis->addItem(objectName(item), item.id);
        }
    }
    form->addRow(tr("Axis"), axis);
    auto* offset = new QDoubleSpinBox(&dialog);
    offset->setObjectName(QStringLiteral("revolveOffset"));
    offset->setRange(-100000, 100000);
    offset->setSuffix(tr(" mm"));
    form->addRow(tr("Offset perpendicular to axis"), offset);
    connect(axis, &QComboBox::currentIndexChanged, offset, [axis, offset] { offset->setEnabled(axis->currentData().toInt() == 0); });
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
        addFeature(tr("Revolve"), [this, id, direction, offset, angle, axis] {
            return document_.addRevolve(id, direction, offset->value(), angle->value(), axis->currentData().toInt());
        });
    }
}

void MainWindow::combineCurrent(geometry::BooleanOperation operation)
{
    const auto first = selectedId();
    const auto* current = document_.find(first);
    if (!current || current->status != document::Status::Ready
        || !document::isBody(current->parameters)) {
        return;
    }
    QStringList names;
    QList<int> candidates;
    for (const auto& object : document_.objects()) {
        if (object.id != first && object.status == document::Status::Ready
            && document::isBody(object.parameters)) {
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

void MainWindow::updateFileState()
{
    setWindowTitle(projectPath_.isEmpty() ? tr("Graph3D[*]")
        : tr("Graph3D — %1[*]").arg(QFileInfo(projectPath_).fileName()));
    setWindowModified(document_.revision() != savedRevision_);
}

bool MainWindow::saveProject(bool choosePath)
{
    auto path = projectPath_;
    if (choosePath || path.isEmpty()) {
        QFileDialog dialog(this, tr("Save Graph3D project"), path, tr("Graph3D projects (*.graph3d)"));
        dialog.setOption(QFileDialog::DontUseNativeDialog, darkThemeEnabled() || russianInterface());
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix(QStringLiteral("graph3d"));
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
            return false;
        }
        path = dialog.selectedFiles().constFirst();
    }
    try {
        if (!path.endsWith(QStringLiteral(".graph3d"), Qt::CaseInsensitive)) {
            throw std::invalid_argument("Use the .graph3d extension for a project.");
        }
        io::saveProject(path, document_);
        projectPath_ = path;
        savedRevision_ = document_.revision();
        updateFileState();
        startPage_->rememberProject(path);
        report(tr("Saved %1").arg(QFileInfo(path).fileName()));
        return true;
    } catch (const std::exception& error) {
        const auto message = tr("Could not save project: %1").arg(localizedDiagnostic(error.what()));
        showSceneError(message);
        QMessageBox::warning(this, tr("Save project"), message);
        return false;
    }
}

bool MainWindow::confirmSave()
{
    if (document_.revision() == savedRevision_) {
        return true;
    }
    const auto selection = QMessageBox::warning(this, tr("Unsaved changes"),
        tr("Save changes to the current Graph3D project?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return selection == QMessageBox::Discard || (selection == QMessageBox::Save && saveProject(false));
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (pages_->currentWidget() == sketchEditor_ && QMessageBox::question(this, tr("Unfinished sketch"),
            tr("Discard the unfinished sketch?"), QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    if (confirmSave() && (!draftWindow_ || draftWindow_->close())) {
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::openProject()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Open Graph3D project"), projectPath_,
                                                  tr("Graph3D files (*.graph3d *.g3draw);;3D projects (*.graph3d);;2D drawings (*.g3draw)"), nullptr,
                                                  (darkThemeEnabled() || russianInterface()) ? QFileDialog::DontUseNativeDialog : QFileDialog::Options{});
    if (!path.isEmpty()) {
        openProjectPath(path);
    }
}

void MainWindow::showDraft(const QString& path)
{
    if (!draftWindow_) {
        draftWindow_ = new DraftWindow(this);
        connect(draftWindow_, &DraftWindow::fileUsed, startPage_, &StartPage::rememberProject);
        if (!path.isEmpty() && !draftWindow_->openDrawing(path)) {
            draftWindow_->deleteLater();
            draftWindow_.clear();
            return;
        }
    } else if (path.isEmpty() ? !draftWindow_->newDrawing() : !draftWindow_->openDrawing(path)) {
        return;
    }
    draftWindow_->showNormal();
    draftWindow_->raise();
    draftWindow_->activateWindow();
}

void MainWindow::openProjectPath(const QString& path, bool example)
{
    if (!example && path.endsWith(QStringLiteral(".g3draw"), Qt::CaseInsensitive)) {
        showDraft(path);
        return;
    }
    try {
        auto loaded = io::openProject(path);
        if (!confirmSave() || !showWorkspace()) {
            return;
        }
        {
            const QSignalBlocker block(tree_);
            items_.clear();
            tree_->clear();
        }
        viewport_->syncDocument(document::Document{});
        document_ = std::move(loaded);
        projectPath_ = example ? QString{} : path;
        savedRevision_ = document_.revision();
        refreshDocument(0);
        viewport_->resetCamera();
        viewport_->fitScene();
        if (!example) {
            startPage_->rememberProject(path);
        }
        const auto errors = std::count_if(document_.objects().begin(), document_.objects().end(),
            [](const auto& item) { return item.status != document::Status::Ready; });
        report(errors ? tr("Opened %1. %2 operations need attention in the model tree.")
                         .arg(QFileInfo(path).fileName()).arg(errors)
                      : tr("Opened %1").arg(QFileInfo(path).fileName()));
    } catch (const std::exception& error) {
        const auto message = tr("Could not open project: %1").arg(localizedDiagnostic(error.what()));
        showSceneError(message);
        QMessageBox::warning(this, tr("Open project"), message);
    }
}

void MainWindow::importStep()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Import STEP"), projectPath_,
                                                  tr("STEP files (*.step *.stp)"), nullptr,
                                                  (darkThemeEnabled() || russianInterface()) ? QFileDialog::DontUseNativeDialog : QFileDialog::Options{});
    if (!path.isEmpty()) {
        addFeature(tr("Import STEP"), [this, &path] {
            const auto name = QFileInfo(path).fileName().toUtf8().toStdString();
            return document_.addImported(name, io::readStep(path));
        });
    }
}

void MainWindow::exportModel(io::ExchangeFormat format)
{
    const bool step = format == io::ExchangeFormat::Step;
    const auto filter = step ? tr("STEP files (*.step *.stp)") : tr("STL files (*.stl)");
    auto path = projectPath_;
    if (!path.isEmpty()) {
        const QFileInfo info(path);
        path = info.dir().filePath(info.completeBaseName());
    }
    QFileDialog dialog(this, tr("Export visible bodies"), path, filter);
    dialog.setOption(QFileDialog::DontUseNativeDialog, darkThemeEnabled() || russianInterface());
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(step ? QStringLiteral("step") : QStringLiteral("stl"));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return;
    }
    path = dialog.selectedFiles().constFirst();
    try {
        const auto suffix = QFileInfo(path).suffix().toLower();
        if (step ? suffix != "step" && suffix != "stp" : suffix != "stl") {
            throw std::invalid_argument("Use the extension for the selected export format.");
        }
        io::exportVisible(path, document_, format);
        report(tr("Exported %1").arg(QFileInfo(path).fileName()));
    } catch (const std::exception& error) {
        showSceneError(tr("Could not export model: %1").arg(localizedDiagnostic(error.what())));
    }
}

void MainWindow::startMachining(Machining operation)
{
    const auto id = selectedId();
    const auto* object = document_.find(id);
    if (!object || !object->visible || object->status != document::Status::Ready
        || !document::isBody(object->parameters)) {
        return;
    }
    const auto pick = operation == Machining::SketchFace ? SubshapePick::SketchFace
        : operation == Machining::Hole ? SubshapePick::Face
        : operation == Machining::MeasureVertices ? SubshapePick::Vertex
        : operation == Machining::ChamferSet || operation == Machining::FilletSet ? SubshapePick::EdgeSet
        : SubshapePick::Edge;
    if (viewport_->beginSubshapePick(id, pick)) {
        pendingMachining_ = operation;
        updateActions();
        statusBar()->showMessage(pick == SubshapePick::SketchFace ? tr("Click a planar face for the sketch. Esc cancels.")
            : pick == SubshapePick::Face ? tr("Click the hole center on a planar face. Esc cancels.")
            : pick == SubshapePick::Vertex ? tr("Pick the first vertex on the selected body. Esc cancels.")
            : pick == SubshapePick::EdgeSet ? tr("Select edges and press Enter to confirm. Esc cancels.")
            : tr("Click an edge of the selected body. Esc cancels."));
    }
}

void MainWindow::endMachining(int id)
{
    pendingMachining_.reset();
    pickedEdges_.clear();
    pickedVertex_.reset();
    viewport_->selectObject(id);
    updateActions();
}

void MainWindow::transformCurrent(const QString& title, const geometry::BodyTransform& defaults)
{
    const int id = selectedId();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("transformDialog"));
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    auto* editor = new TransformEditor(&dialog);
    editor->setParameters(defaults);
    layout->addWidget(editor);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        const auto parameters = editor->parameters();
        addFeature(title, [this, id, parameters] { return document_.addTransform(id, parameters); });
    }
}

void MainWindow::configureAssembly(int id)
{
    const auto* object = document_.find(id);
    const auto* existing = object ? std::get_if<document::Assembly>(&object->parameters) : nullptr;
    if (id && !existing) {
        return;
    }
    AssemblyDialog::Sources sources;
    for (const auto& candidate : document_.objects()) {
        if (existing && candidate.id >= id) {
            break;
        }
        if (document::isBody(candidate.parameters)
            && (existing || candidate.status == document::Status::Ready)) {
            sources.append({candidate.id, objectName(candidate)});
        }
    }
    if (sources.isEmpty()) {
        return;
    }
    AssemblyDialog dialog(std::move(sources), existing, this);
    if (dialog.exec() == QDialog::Accepted) {
        const auto assembly = dialog.assembly();
        if (existing) {
            editFeature(id, [this, id, &assembly] { return document_.editAssembly(id, assembly); });
        } else {
            addFeature(tr("Create assembly"), [this, &assembly] { return document_.addAssembly(assembly); });
        }
    }
}

void MainWindow::makeHole(int id, const geometry::FaceReference& face)
{
    if (pendingMachining_ == Machining::SketchFace) {
        endMachining(id);
        geometry::Sketch sketch;
        sketch.support = geometry::SketchSupport{id, face};
        beginSketch(sketch);
        return;
    }
    if (pendingMachining_ != Machining::Hole) {
        return;
    }
    endMachining(id);
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("holeDialog"));
    dialog.setWindowTitle(tr("Hole"));
    auto* form = new QFormLayout(&dialog);
    auto* diameter = new QDoubleSpinBox(&dialog);
    diameter->setObjectName(QStringLiteral("holeDiameter"));
    diameter->setRange(0.01, 100000);
    diameter->setValue(10);
    diameter->setSuffix(tr(" mm"));
    form->addRow(tr("Diameter"), diameter);
    auto* through = new QCheckBox(tr("Through"), &dialog);
    through->setObjectName(QStringLiteral("holeThrough"));
    through->setChecked(true);
    form->addRow(through);
    auto* depth = new QDoubleSpinBox(&dialog);
    depth->setObjectName(QStringLiteral("holeDepth"));
    depth->setRange(0.01, 100000);
    depth->setValue(10);
    depth->setSuffix(tr(" mm"));
    depth->setEnabled(false);
    form->addRow(tr("Blind depth"), depth);
    connect(through, &QCheckBox::toggled, depth, &QDoubleSpinBox::setDisabled);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        const geometry::HoleSize size{diameter->value(), depth->value(), through->isChecked()};
        addFeature(tr("Hole"), [this, id, face, size] { return document_.addHole(id, face, size); });
    } else {
        statusBar()->showMessage(tr("Hole cancelled"));
    }
}

void MainWindow::makeEdgeTreatment(int id, const geometry::EdgeReference& edge)
{
    if (pendingMachining_ == Machining::ChamferSet || pendingMachining_ == Machining::FilletSet) {
        const auto match = std::ranges::find(pickedEdges_, edge);
        if (match == pickedEdges_.end()) {
            pickedEdges_.push_back(edge);
        } else {
            pickedEdges_.erase(match);
        }
        statusBar()->showMessage(tr("Selected edges: %1. Press Enter to confirm.").arg(pickedEdges_.size()));
        updateActions();
        return;
    }
    if (pendingMachining_ != Machining::Chamfer && pendingMachining_ != Machining::Fillet) {
        return;
    }
    const auto kind = pendingMachining_ == Machining::Chamfer
        ? geometry::EdgeTreatmentKind::Chamfer : geometry::EdgeTreatmentKind::Fillet;
    endMachining(id);
    applyEdgeTreatment(id, kind, {edge});
}

void MainWindow::finishEdgeSet()
{
    if (pickedEdges_.empty() || (pendingMachining_ != Machining::ChamferSet && pendingMachining_ != Machining::FilletSet)) {
        return;
    }
    if (pickedEdges_.size() > 64) {
        statusBar()->showMessage(tr("Select at most 64 edges."));
        return;
    }
    const auto kind = pendingMachining_ == Machining::ChamferSet
        ? geometry::EdgeTreatmentKind::Chamfer : geometry::EdgeTreatmentKind::Fillet;
    auto edges = std::move(pickedEdges_);
    const int id = selectedId();
    viewport_->cancelSubshapePick();
    applyEdgeTreatment(id, kind, std::move(edges));
}

void MainWindow::applyEdgeTreatment(int id, geometry::EdgeTreatmentKind kind, std::vector<geometry::EdgeReference> edges)
{
    const bool chamfer = kind == geometry::EdgeTreatmentKind::Chamfer;
    const auto title = chamfer ? tr("Chamfer") : tr("Fillet");
    bool accepted = false;
    const auto value = QInputDialog::getDouble(this, title,
        chamfer ? tr("Equal distances (mm)") : tr("Radius (mm)"), 2, 0.01, 100000, 2, &accepted);
    if (accepted) {
        addFeature(title, [this, id, &edges, kind, value] {
            return document_.addEdgeTreatment(id, std::move(edges), kind, value);
        });
    } else {
        statusBar()->showMessage(tr("%1 cancelled").arg(title));
    }
}

void MainWindow::configureAdvanced(const QString& title, const document::Parameters& defaults, int editing)
{
    if (pages_->currentWidget() != viewport_ || pendingMachining_) {
        return;
    }
    try {
        const auto result = advancedParameters(this, title, document_, defaults, editing != 0);
        if (!result) {
            return;
        }
        if (editing) {
            editFeature(editing, [this, editing, &result] { return document_.editAdvanced(editing, *result); });
        } else {
            addFeature(title, [this, &result] { return document_.addAdvanced(*result); });
        }
    } catch (const std::exception& error) {
        showSceneError(localizedDiagnostic(error.what()));
    } catch (const Standard_Failure& error) {
        showSceneError(localizedDiagnostic(error.GetMessageString()));
    }
}

void MainWindow::measureBody()
{
    const auto* body = document_.find(selectedId());
    if (!body || body->status != document::Status::Ready || !document::isBody(body->parameters)) {
        return;
    }
    const int id = body->id;
    try {
        if (const auto value = bodyPropertiesDialog(this, *body)) {
            if (document_.setDensity(id, *value > 0 ? value : std::nullopt)) {
                refreshDocument(id);
                report(tr("Material density saved."));
            }
        }
    } catch (const std::exception& error) {
        showSceneError(localizedDiagnostic(error.what()));
    } catch (const Standard_Failure& error) {
        showSceneError(localizedDiagnostic(error.GetMessageString()));
    }
}

void MainWindow::measureBodyDistance()
{
    const auto* selected = document_.find(selectedId());
    if (!selected || selected->status != document::Status::Ready || !document::isBody(selected->parameters)) {
        return;
    }
    QStringList names;
    std::vector<int> ids;
    for (const auto& body : document_.objects()) {
        if (body.id != selected->id && body.status == document::Status::Ready && document::isBody(body.parameters)) {
            names << objectName(body);
            ids.push_back(body.id);
        }
    }
    bool accepted = false;
    const auto title = tr("Distance between bodies");
    const auto chosen = QInputDialog::getItem(this, title, tr("Second body"), names, 0, false, &accepted);
    if (!accepted) {
        return;
    }
    try {
        const auto* other = document_.find(ids.at(names.indexOf(chosen)));
        const auto distance = geometry::minimumDistance(selected->shape, other->shape);
        const auto result = tr("Minimum distance: %1 mm").arg(distance, 0, 'g', 10);
        report(result);
        QMessageBox::information(this, title, result);
    } catch (const std::exception& error) {
        showSceneError(localizedDiagnostic(error.what()));
    } catch (const Standard_Failure& error) {
        showSceneError(localizedDiagnostic(error.GetMessageString()));
    }
}

void MainWindow::showDrawing()
{
    const auto* object = document_.find(selectedId());
    if (!object || object->status != document::Status::Ready
        || !document::isBody(object->parameters)) {
        return;
    }
    try {
        const auto create = [object] {
            struct WaitCursor {
                WaitCursor() { QApplication::setOverrideCursor(Qt::WaitCursor); }
                ~WaitCursor() { QApplication::restoreOverrideCursor(); }
            } cursor;
            return drawing::project(object->shape);
        };
        DrawingDialog dialog(object->shape, create(), objectName(*object), object->id, this);
        dialog.exec();
    } catch (const std::exception& error) {
        const auto message = tr("Could not create drawing: %1").arg(localizedDiagnostic(error.what()));
        showSceneError(message);
        QMessageBox::warning(this, tr("Drawing"), message);
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
