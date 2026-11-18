#include "gui/MainWindow.h"
#include "gui/DocumentWindow.h"

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
#include "io/LinkedComponents.h"
#include "io/SpecificationIO.h"
#include "io/ProjectIO.h"
#include "viewport/Viewport.h"

#include <Standard_Failure.hxx>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <Quantity_Color.hxx>
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
#include <QLineEdit>
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
#include <tuple>

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

MainWindow::MainWindow(DocumentWindow* host)
    : QMainWindow(host), host_(host), viewport_(new Viewport(this)), pages_(new QStackedWidget(this)),
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
            auto size = std::get<document::Hole>(document_.find(id)->parameters).size;
            size.diameter = diameter; size.depth = depth; size.through = through;
            return document_.editHole(id, size);
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
    connect(viewport_, &Viewport::blendPicked, this, [this](int id, const geometry::BlendReference& face) {
        if (pendingMachining_ != Machining::RemoveBlend) { return; }
        endMachining(id);
        addFeature(tr("Remove blend"), [this, id, face] { return document_.addAdvanced(document::RemoveBlend{id, face}); });
    });
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
    connect(startPageAction_, &QAction::triggered, this, [this] {
        if (host_) {
            host_->showHome();
        } else {
            showStartPage();
        }
    });
    file->addSeparator();
    newScene_ = menuAction(file, host_ ? tr("New part") : tr("&New scene"), "newScene");
    newScene_->setShortcut(QKeySequence::New);
    connect(newScene_, &QAction::triggered, this, [this] {
        if (host_) {
            host_->newProject();
            return;
        }
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
    connect(import, &QAction::triggered, this, [this] { importModel(io::ExchangeFormat::Step); });
    auto* importIges = menuAction(file, tr("Import IGES…"), "importIges");
    connect(importIges, &QAction::triggered, this, [this] { importModel(io::ExchangeFormat::Iges); });
    auto* importStl = menuAction(file, tr("Import STL…"), "importStl");
    connect(importStl, &QAction::triggered, this, [this] { importModel(io::ExchangeFormat::Stl); });
    auto* exportStep = menuAction(file, tr("Export visible geometry to STEP…"), "exportStep");
    connect(exportStep, &QAction::triggered, this, [this] { exportModel(io::ExchangeFormat::Step); });
    auto* exportStl = menuAction(file, tr("Export visible bodies to STL…"), "exportStl");
    connect(exportStl, &QAction::triggered, this, [this] { exportModel(io::ExchangeFormat::Stl); });
    auto* exportIges = menuAction(file, tr("Export visible bodies to IGES…"), "exportIges");
    connect(exportIges, &QAction::triggered, this, [this] { exportModel(io::ExchangeFormat::Iges); });
    fileActions_ = {save, saveAs, import, importIges, importStl, exportStep, exportStl, exportIges};

    file->addSeparator();
    auto* quit = file->addAction(tr("E&xit"), this, [this] {
        if (host_) {
            host_->close();
        } else {
            close();
        }
    });
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
    auto* metal = model->addMenu(tr("Sheet metal"));
    auto* surfaces = model->addMenu(tr("Curves and surfaces"));
    connect(menuAction(surfaces, tr("Spatial curve…"), "spatialCurve"), &QAction::triggered,
        this, [this] { configureSpatialCurve(); });
    for (const auto& [kind, text, name] : {
        std::tuple{geometry::SurfaceKind::Loft, tr("Surface through sections…"), "surfaceLoft"},
        {geometry::SurfaceKind::Sweep, tr("Sweep surface…"), "surfaceSweep"},
        {geometry::SurfaceKind::Network, tr("Surface from curve network…"), "surfaceNetwork"},
        {geometry::SurfaceKind::Trim, tr("Trim surface…"), "surfaceTrim"},
        {geometry::SurfaceKind::Extend, tr("Extend surface…"), "surfaceExtend"},
        {geometry::SurfaceKind::Patch, tr("Patch boundary…"), "surfacePatch"},
        {geometry::SurfaceKind::Sew, tr("Sew surfaces…"), "surfaceSew"},
        {geometry::SurfaceKind::Solid, tr("Create solid from surfaces…"), "surfaceSolid"},
        {geometry::SurfaceKind::OpenEdges, tr("Show open edges…"), "surfaceOpenEdges"}}) {
        connect(menuAction(surfaces, text, name), &QAction::triggered, this, [this, kind] { configureSurface(kind); });
    }
    auto* annotations = model->addMenu(tr("3D annotations"));
    for (const auto& [kind, text, name] : {
        std::tuple{document::AnnotationKind::Linear, tr("Linear dimension…"), "pmiLinear"},
        {document::AnnotationKind::Diameter, tr("Hole diameter…"), "pmiDiameter"},
        {document::AnnotationKind::Datum, tr("Datum on face…"), "pmiDatum"},
        {document::AnnotationKind::Roughness, tr("Surface roughness…"), "pmiRoughness"}}) {
        connect(menuAction(annotations, text, name), &QAction::triggered, this, [this, kind] { configureAnnotation(kind); });
    }
    connect(menuAction(metal,tr("New sheet part…"),"newSheetMetal"),&QAction::triggered,this,[this]{configureSheetMetal();});
    connect(menuAction(metal,tr("Edit sheet part…"),"editSheetMetal"),&QAction::triggered,this,[this]{if(selectedId()) configureSheetMetal(selectedId());});
    connect(menuAction(metal,tr("Create flat drawing…"),"flatDrawing"),&QAction::triggered,this,[this]{showFlatSheet(false);});
    connect(menuAction(metal,tr("Export flat DXF…"),"flatDxf"),&QAction::triggered,this,[this]{showFlatSheet(true);});
    variables_ = menuAction(model, tr("Variables and variants…"), "modelVariables");
    connect(variables_, &QAction::triggered, this, &MainWindow::editVariables);
    model->addSeparator();
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
    machiningAction(tr("Move flat face…"), "moveFlatFace", Machining::MoveFace);
    machiningAction(tr("Remove blend…"), "removeBlend", Machining::RemoveBlend);
    machiningAction(tr("Chamfer…"), "makeChamfer", Machining::Chamfer);
    machiningAction(tr("Fillet…"), "makeFillet", Machining::Fillet);
    machiningAction(tr("Chamfer multiple edges…"), "makeMultiChamfer", Machining::ChamferSet);
    machiningAction(tr("Fillet multiple edges…"), "makeMultiFillet", Machining::FilletSet);
    featurePattern_ = menuAction(model, tr("Pattern individual operation…"), "patternOperation");
    connect(featurePattern_, &QAction::triggered, this, [this] { configureFeaturePattern(selectedId()); });
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
        const auto* profile = object ? std::get_if<geometry::Sketch>(&object->parameters) : nullptr;
        if (profile && !profile->trajectory && object->status == document::Status::Ready) {
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
        if (object && (document::isAdvanced(object->parameters) || std::holds_alternative<document::Annotation3d>(object->parameters))) {
            configureAdvanced(objectName(*object), object->parameters, object->id);
        }
    });
    auto* measure = model->addMenu(tr("Measure"));
    connect(menuAction(measure, tr("Assign catalog material…"), "catalogMaterial"), &QAction::triggered,
        this, &MainWindow::chooseCatalogMaterial);
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

    auto* assemblies = model->addMenu(tr("Assemblies"));
    createAssembly_ = menuAction(assemblies, tr("Create assembly…"), "createAssembly");
    connect(createAssembly_, &QAction::triggered, this, [this] { configureAssembly(0); });
    auto* standardParts = menuAction(assemblies, tr("Insert catalog fastener…"), "insertCatalogFastener");
    connect(standardParts, &QAction::triggered, this, [this] { configureFastener(); });
    insertLinked_ = menuAction(assemblies, tr("Insert component from file…"), "insertLinkedComponent");
    updateLinked_ = menuAction(assemblies, tr("Update all linked components"), "updateLinkedComponents");
    updateSelectedLink_ = menuAction(assemblies, tr("Update selected component"), "updateSelectedLink");
    relink_ = menuAction(assemblies, tr("Change component source…"), "relinkComponent");
    partProperties_ = menuAction(assemblies, tr("Component properties…"), "componentProperties");
    interference_ = menuAction(assemblies, tr("Check component interference…"), "checkInterference");
    motion_ = menuAction(assemblies, tr("Move and check assembly…"), "assemblyMotion");
    assemblySpecification_ = menuAction(assemblies, tr("Create specification document…"), "createAssemblySpecification");
    assemblyStructure_ = menuAction(assemblies, tr("Assembly structure…"), "assemblyStructure");
    connect(insertLinked_, &QAction::triggered, this, [this] { insertLinkedComponent(false); });
    connect(updateLinked_, &QAction::triggered, this, [this] { updateLinkedComponents(false); });
    connect(updateSelectedLink_, &QAction::triggered, this, [this] { updateLinkedComponents(true); });
    connect(relink_, &QAction::triggered, this, [this] { insertLinkedComponent(true); });
    connect(partProperties_, &QAction::triggered, this, &MainWindow::editPartProperties);
    connect(interference_, &QAction::triggered, this, &MainWindow::showAssemblyInterference);
    connect(motion_, &QAction::triggered, this, &MainWindow::moveAssembly);
    connect(assemblySpecification_, &QAction::triggered, this, &MainWindow::createAssemblySpecification);
    connect(assemblyStructure_, &QAction::triggered, this, &MainWindow::showAssemblyStructure);

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
        if (host_) {
            host_->setDarkTheme(dark);
        }
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
    connect(languageMenu, &QMenu::aboutToShow, this, [languages] {
        languages->actions().at(russianLanguagePreferred() ? 1 : 0)->setChecked(true);
    });

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
        if (object && std::holds_alternative<document::Assembly>(object->parameters)) {
            configureAssembly(id);
        } else if (object && std::holds_alternative<geometry::Fastener>(object->parameters)) {
            configureFastener(id);
        } else if (object && std::holds_alternative<geometry::SheetMetal>(object->parameters)) {
            configureSheetMetal(id);
        } else if (object && std::holds_alternative<document::Hole>(object->parameters)) {
            const auto& hole = std::get<document::Hole>(object->parameters);
            configureHole(hole.body, hole.face, hole.size, id);
        } else if (object && std::holds_alternative<document::EdgeTreatment>(object->parameters)) {
            configureEdgeTreatment(std::get<document::EdgeTreatment>(object->parameters), id);
        } else if (object && (document::isAdvanced(object->parameters) || std::holds_alternative<document::Annotation3d>(object->parameters))) {
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
                } else if (const auto* pattern = std::get_if<document::FeaturePattern>(&object.parameters)) {
                    parentId = pattern->seed;
                } else if (std::holds_alternative<document::MoveFace>(object.parameters)
                    || std::holds_alternative<document::RemoveBlend>(object.parameters)) {
                    parentId = document::featureBase(object.parameters);
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
    const auto* sketch = object ? std::get_if<geometry::Sketch>(&object->parameters) : nullptr;
    const bool profile = sketch && !sketch->trajectory;
    const auto validBody = [](const document::Object& candidate) {
        return candidate.status == document::Status::Ready
            && document::isBody(candidate.parameters);
    };
    const auto bodies = std::count_if(document_.objects().begin(), document_.objects().end(), validBody);
    const auto sketches = std::ranges::count_if(document_.objects(), [](const auto& item) {
        const auto* sketch = std::get_if<geometry::Sketch>(&item.parameters);
        return item.status == document::Status::Ready && sketch && !sketch->trajectory;
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
    variables_->setEnabled(available);
    featurePattern_->setEnabled(available && object && validBody(*object) && document::featureBase(object->parameters));
    const bool selectedBody = object && validBody(*object);
    newDatumSketch_->setEnabled(available && object && object->status == document::Status::Ready
        && std::holds_alternative<geometry::DatumPlane>(object->parameters));
    sweep_->setEnabled(available && profile && object->status == document::Status::Ready);
    loft_->setEnabled(available && sketches >= 2);
    shell_->setEnabled(available && selectedBody);
    draftAngle_->setEnabled(available && selectedBody);
    editAdvanced_->setEnabled(available && object && (document::isAdvanced(object->parameters)
        || std::holds_alternative<document::Annotation3d>(object->parameters)));
    measureBody_->setEnabled(available && selectedBody);
    measureDistance_->setEnabled(available && selectedBody && bodies >= 2);
    finishEdgeSet_->setEnabled(modeling && !pickedEdges_.empty()
        && (pendingMachining_ == Machining::ChamferSet || pendingMachining_ == Machining::FilletSet));
    newFaceSketch_->setEnabled(modeling && !pendingMachining_ && object && object->visible && validBody(*object));
    extrude_->setEnabled(modeling && profile && object->status == document::Status::Ready);
    revolve_->setEnabled(modeling && profile && object->status == document::Status::Ready);
    booleanMenu_->setEnabled(modeling && object && validBody(*object) && bodies >= 2);
    transformMenu_->setEnabled(modeling && !pendingMachining_ && object && validBody(*object));
    createAssembly_->setEnabled(available && bodies > 0);
    const auto* linked = object ? std::get_if<document::LinkedComponent>(&object->parameters) : nullptr;
    const bool hasLinks = std::ranges::any_of(document_.objects(), [](const auto& candidate) {
        return std::holds_alternative<document::LinkedComponent>(candidate.parameters);
    });
    const bool assemblyObject = object && (std::holds_alternative<document::Assembly>(object->parameters)
        || (linked && !linked->record.children.empty()));
    insertLinked_->setEnabled(available);
    updateLinked_->setEnabled(available && hasLinks);
    updateSelectedLink_->setEnabled(available && linked);
    relink_->setEnabled(available && linked);
    partProperties_->setEnabled(available && object && document::isBody(object->parameters));
    interference_->setEnabled(available && assemblyObject && object->status == document::Status::Ready);
    motion_->setEnabled(available && object && std::holds_alternative<document::Assembly>(object->parameters)
        && object->status == document::Status::Ready);
    assemblySpecification_->setEnabled(host_ && available && assemblyObject && object->status == document::Status::Ready);
    assemblyStructure_->setEnabled(available && assemblyObject && object->status == document::Status::Ready);
    drawing_->setEnabled(modeling && object && validBody(*object));
    for (auto* action : machiningActions_) {
        action->setEnabled(modeling && !pendingMachining_ && object && object->visible && validBody(*object));
    }
}

QString MainWindow::objectName(const document::Object& object) const
{
    const auto name = std::visit([this, &object](const auto& operation) -> QString {
        using Type = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Type, document::Primitive>) {
            switch (operation.kind) {
            case geometry::Primitive::Cube: return tr("Cube");
            case geometry::Primitive::Cylinder: return tr("Cylinder");
            case geometry::Primitive::Sphere: return tr("Sphere");
            }
        } else if constexpr (std::is_same_v<Type, geometry::Sketch>) {
            return operation.trajectory ? tr("Path sketch") : tr("Sketch");
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
        } else if constexpr (std::is_same_v<Type, document::LinkedComponent>) {
            const auto name = object.properties.name.empty() ? operation.record.properties.name : object.properties.name;
            return name.empty() ? tr("Linked component") : QString::fromStdString(name);
        } else if constexpr (std::is_same_v<Type, document::Imported>) {
            return tr("Imported body");
        } else if constexpr (std::is_same_v<Type, document::Hole>) {
            if (operation.size.style == geometry::HoleStyle::Counterbore) { return tr("Counterbore"); }
            if (operation.size.style == geometry::HoleStyle::Countersink) { return tr("Countersink"); }
            return tr("Hole");
        } else if constexpr (std::is_same_v<Type, document::FeaturePattern>) {
            return tr("Operation pattern");
        } else if constexpr (std::is_same_v<Type, document::MoveFace>) {
            return tr("Move flat face");
        } else if constexpr (std::is_same_v<Type, document::RemoveBlend>) {
            return tr("Remove blend");
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
            return object.properties.name.empty() ? tr("Assembly") : QString::fromStdString(object.properties.name);
        } else if constexpr (std::is_same_v<Type, geometry::SheetMetal>) {
            return tr("Sheet metal");
        } else if constexpr (std::is_same_v<Type, document::ImportedMesh>) {
            return tr("Imported mesh");
        } else if constexpr (std::is_same_v<Type, geometry::SpatialCurve>) {
            return operation.spline ? tr("Spatial spline") : tr("Spatial polyline");
        } else if constexpr (std::is_same_v<Type, document::ImportedSurface>) {
            return tr("Imported free geometry");
        } else if constexpr (std::is_same_v<Type, document::Surface>) {
            const std::array names{tr("Surface loft"), tr("Surface sweep"), tr("Curve network"), tr("Trimmed surface"),
                tr("Extended surface"), tr("Patch"), tr("Sewn shell"), tr("Solid from surfaces"), tr("Open edges")};
            return names.at(static_cast<std::size_t>(operation.kind));
        } else if constexpr (std::is_same_v<Type, document::Annotation3d>) {
            return tr("3D annotation");
        } else if constexpr (std::is_same_v<Type, geometry::Fastener>) {
            return operation.kind == geometry::FastenerKind::Bolt ? tr("Hex bolt")
                : operation.kind == geometry::FastenerKind::Nut ? tr("Hex nut") : tr("Flat washer");
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
    auto parameters = object.parameters;
    if (object.status == document::Status::Ready && document_.configuration().bindings.contains(object.id)) {
        parameters = document_.effectiveParameters(object.id);
    }
    return std::visit([this, &object](const auto& operation) -> QString {
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
        } else if constexpr (std::is_same_v<Type, document::LinkedComponent>) {
            return tr("%1 · object #%2").arg(QDir::toNativeSeparators(QString::fromStdString(operation.sourcePath))).arg(operation.sourceObject);
        } else if constexpr (std::is_same_v<Type, document::Imported>) {
            return QString::fromStdString(operation.name);
        } else if constexpr (std::is_same_v<Type, document::ImportedSurface>) {
            return QString::fromStdString(operation.name);
        } else if constexpr (std::is_same_v<Type, document::Surface>) {
            QStringList sources;
            for (const auto id : operation.sources) { sources << QString::number(id); }
            return sources.join(QStringLiteral(", "));
        } else if constexpr (std::is_same_v<Type, document::Annotation3d>) {
            const auto value = document_.resolveAnnotation(object.id);
            if (!value) { return tr("Lost 3D reference"); }
            if (value->kind == document::AnnotationKind::Linear || value->kind == document::AnnotationKind::Diameter) {
                return (value->kind == document::AnnotationKind::Diameter ? QStringLiteral("Ø") : QString{})
                    + tr("%1 mm").arg(value->value, 0, 'g', 8);
            }
            return QString::fromStdString(value->text);
        } else if constexpr (std::is_same_v<Type, document::ImportedMesh>) {
            return tr("%1 · %2 triangles").arg(QString::fromStdString(operation.name)).arg(operation.vertices.size()/3);
        } else if constexpr (std::is_same_v<Type, document::Hole>) {
            const auto mode = operation.size.through ? tr("Through")
                : tr("Depth %1 mm").arg(operation.size.depth);
            return tr("%1 · ⌀%2 mm · %3").arg(source(operation.body)).arg(operation.size.diameter).arg(mode);
        } else if constexpr (std::is_same_v<Type, document::EdgeTreatment>) {
            const auto size = operation.kind == geometry::EdgeTreatmentKind::Chamfer
                ? tr("Distance %1 mm").arg(operation.size) : tr("Radius %1 mm").arg(operation.size);
            return tr("%1 · %2 · %3 edges").arg(source(operation.body), size).arg(operation.additionalEdges.size() + 1);
        } else if constexpr (std::is_same_v<Type, document::Sweep>) {
            return operation.pathSketch ? tr("%1 along %2").arg(source(operation.profile), source(operation.pathSketch))
                : tr("%1 · %2 path points").arg(source(operation.profile)).arg(operation.path.size());
        } else if constexpr (std::is_same_v<Type, document::Loft>) {
            return tr("Sections: %1").arg(operation.profiles.size());
        } else if constexpr (std::is_same_v<Type, document::Shell>) {
            return tr("%1 · %2 mm wall").arg(source(operation.body)).arg(operation.thickness);
        } else if constexpr (std::is_same_v<Type, document::DraftAngle>) {
            return tr("%1 · %2°").arg(source(operation.body)).arg(operation.degrees);
        } else if constexpr (std::is_same_v<Type, document::FeaturePattern>) {
            return tr("%1 · %2 instances").arg(source(operation.seed)).arg(operation.count);
        } else if constexpr (std::is_same_v<Type, document::MoveFace>) {
            return tr("%1 · %2 mm").arg(source(operation.body)).arg(operation.distance);
        } else if constexpr (std::is_same_v<Type, document::RemoveBlend>) {
            return source(operation.body);
        } else if constexpr (std::is_same_v<Type, document::Transform>) {
            return source(operation.body);
        } else if constexpr (std::is_same_v<Type, document::Assembly>) {
            return tr("Components: %1").arg(operation.components.size());
        } else if constexpr (std::is_same_v<Type, geometry::Fastener>) {
            return QString::fromStdString(geometry::fastenerDesignation(operation));
        }
        return {};
    }, parameters);
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
    if (host_) {
        host_->documentChanged(this);
    }
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
        if (host_ && !host_->canSaveAs(this, path)) {
            return false;
        }
        io::saveProject(path, document_);
        projectPath_ = path;
        savedRevision_ = document_.revision();
        updateFileState();
        startPage_->rememberProject(path);
        if (host_) {
            host_->rememberProject(path);
        }
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

bool MainWindow::canClose()
{
    if (pages_->currentWidget() == sketchEditor_ && QMessageBox::question(this, tr("Unfinished sketch"),
            tr("Discard the unfinished sketch?"), QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return false;
    }
    return confirmSave();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (canClose() && (!draftWindow_ || draftWindow_->close())) {
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::openProject()
{
    if (host_) {
        host_->browse();
        return;
    }
    const auto path = QFileDialog::getOpenFileName(this, tr("Open Graph3D project"), projectPath_,
                                                  tr("Graph3D files (*.graph3d *.g3draw);;3D projects (*.graph3d);;2D drawings (*.g3draw)"), nullptr,
                                                  (darkThemeEnabled() || russianInterface()) ? QFileDialog::DontUseNativeDialog : QFileDialog::Options{});
    if (!path.isEmpty()) {
        openProjectPath(path);
    }
}

void MainWindow::showDraft(const QString& path)
{
    if (host_) {
        if (path.isEmpty()) {
            host_->newDrawing();
        } else {
            host_->openDocument(path);
        }
        return;
    }
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

void MainWindow::importModel(io::ExchangeFormat format)
{
    const bool step = format == io::ExchangeFormat::Step;
    const bool stl = format == io::ExchangeFormat::Stl;
    const auto title = step ? tr("Import STEP") : stl ? tr("Import STL") : tr("Import IGES");
    const auto filter = step ? tr("STEP files (*.step *.stp)") : stl ? tr("STL files (*.stl)")
        : tr("IGES files (*.iges *.igs)");
    const auto path = QFileDialog::getOpenFileName(this, title, projectPath_, filter, nullptr,
                                                  (darkThemeEnabled() || russianInterface()) ? QFileDialog::DontUseNativeDialog : QFileDialog::Options{});
    if (path.isEmpty()) {
        return;
    }
    double scale = 1;
    if (stl) {
        bool accepted = false;
        scale = QInputDialog::getDouble(this, title, tr("Millimetres per STL unit"), 1,
                                        .000001, 1000000, 6, &accepted);
        if (!accepted) { return; }
    }
    addFeature(title, [this, &path, step, stl, scale] {
        if (step) { return document_.importTree(io::readStepTree(path)); }
        const auto name = QFileInfo(path).fileName().toUtf8().toStdString();
        if (stl) { return document_.addImportedMesh(io::readStl(path, scale)); }
        return document_.addImported(name, io::readIges(path));
    });
}

void MainWindow::exportModel(io::ExchangeFormat format)
{
    const bool step = format == io::ExchangeFormat::Step;
    const bool stl = format == io::ExchangeFormat::Stl;
    const auto filter = step ? tr("STEP files (*.step *.stp)") : stl ? tr("STL files (*.stl)")
        : tr("IGES files (*.iges *.igs)");
    auto path = projectPath_;
    if (!path.isEmpty()) {
        const QFileInfo info(path);
        path = info.dir().filePath(info.completeBaseName());
    }
    QFileDialog dialog(this, tr("Export visible bodies"), path, filter);
    dialog.setOption(QFileDialog::DontUseNativeDialog, darkThemeEnabled() || russianInterface());
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(step ? QStringLiteral("step") : stl ? QStringLiteral("stl") : QStringLiteral("iges"));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return;
    }
    path = dialog.selectedFiles().constFirst();
    try {
        const auto suffix = QFileInfo(path).suffix().toLower();
        if (step ? suffix != "step" && suffix != "stp"
                 : stl ? suffix != "stl" : suffix != "iges" && suffix != "igs") {
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
        : operation == Machining::Hole || operation == Machining::MoveFace ? SubshapePick::Face
        : operation == Machining::RemoveBlend ? SubshapePick::Blend
        : operation == Machining::MeasureVertices ? SubshapePick::Vertex
        : operation == Machining::ChamferSet || operation == Machining::FilletSet ? SubshapePick::EdgeSet
        : SubshapePick::Edge;
    if (viewport_->beginSubshapePick(id, pick)) {
        pendingMachining_ = operation;
        updateActions();
        statusBar()->showMessage(pick == SubshapePick::SketchFace ? tr("Click a planar face for the sketch. Esc cancels.")
            : operation == Machining::MoveFace ? tr("Click a planar face to move. Esc cancels.")
            : pick == SubshapePick::Blend ? tr("Click a blend face to remove. Esc cancels.")
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
    AssemblyDialog dialog(document_, std::move(sources), existing, this);
    if (dialog.exec() == QDialog::Accepted) {
        const auto assembly = dialog.assembly();
        if (existing) {
            editFeature(id, [this, id, &assembly] { return document_.editAssembly(id, assembly); });
        } else {
            addFeature(tr("Create assembly"), [this, &assembly] { return document_.addAssembly(assembly); });
        }
    }
}

void MainWindow::createAssemblySpecification()
{
    if (!host_ || !selectedId()) { return; }
    if ((projectPath_.isEmpty() || isWindowModified()) && !saveProject(false)) { return; }
    try {
        drawing::SpecificationDocument spec;
        spec.sourcePath = QFileInfo(projectPath_).absoluteFilePath();
        spec.assemblyId = selectedId();
        drawing::mergeSpecification(spec, *io::readSpecificationAssembly(spec));
        host_->createSpecification(std::move(spec));
    } catch (const std::exception& error) {
        showSceneError(localizedDiagnostic(error.what()));
    }
}

void MainWindow::insertLinkedComponent(bool replace)
{
    const int id = selectedId();
    const auto* selected = document_.find(id);
    const auto* old = replace && selected ? std::get_if<document::LinkedComponent>(&selected->parameters) : nullptr;
    if (replace && !old) {
        return;
    }
    const auto title = replace ? tr("Change component source") : tr("Insert component from file");
    const auto path = QFileDialog::getOpenFileName(this, title, old ? QString::fromStdString(old->sourcePath) : projectPath_,
        tr("Graph3D projects (*.graph3d)"), nullptr,
        (darkThemeEnabled() || russianInterface()) ? QFileDialog::DontUseNativeDialog : QFileDialog::Options{});
    if (path.isEmpty()) {
        return;
    }
    try {
        const auto source = io::openProject(path);
        QStringList names;
        std::vector<int> ids;
        for (const auto& candidate : source.objects()) {
            if (document::isBody(candidate.parameters) && candidate.status == document::Status::Ready) {
                const auto record = source.componentRecord(candidate.id);
                const auto name = record.properties.name.empty() ? objectName(candidate)
                    : tr("%1 · object #%2").arg(QString::fromStdString(record.properties.name)).arg(candidate.id);
                names << (candidate.visible ? name : tr("%1 (hidden)").arg(name));
                ids.push_back(candidate.id);
            }
        }
        if (ids.empty()) {
            throw std::invalid_argument("The linked project has no ready solid or assembly.");
        }
        int selectedObject = ids.front();
        if (ids.size() > 1) {
            auto first = std::ranges::find_if(ids, [&](int object) {
                return old ? old->sourceObject == object : source.find(object)->visible;
            });
            const int index = first == ids.end() ? 0 : static_cast<int>(first - ids.begin());
            bool accepted = false;
            const auto chosen = QInputDialog::getItem(this, title, tr("Source object"), names, index, false, &accepted);
            if (!accepted) {
                return;
            }
            selectedObject = ids.at(names.indexOf(chosen));
        }
        auto linked = io::readLinkedComponent(path, selectedObject, projectPath_);
        if (replace) {
            editFeature(id, [&] { return document_.refreshLinkedComponents({{id, std::move(linked)}}); });
        } else {
            addFeature(title, [&] { return document_.addLinkedComponent(std::move(linked)); });
        }
    } catch (const std::exception& error) {
        QMessageBox::warning(this, title, localizedDiagnostic(error.what()));
    } catch (const Standard_Failure& error) {
        QMessageBox::warning(this, title, localizedDiagnostic(error.GetMessageString()));
    }
}

void MainWindow::updateLinkedComponents(bool selected)
{
    const int id = selectedId();
    const auto title = tr("Update linked components");
    try {
        if (!io::updateLinkedComponents(document_, projectPath_, selected ? id : 0)) {
            report(tr("Linked components are up to date."));
            return;
        }
        refreshDocument(id);
        const auto failed = std::ranges::find_if(document_.objects(), [](const auto& object) {
            return std::holds_alternative<document::Assembly>(object.parameters) && object.status == document::Status::Failed;
        });
        report(failed == document_.objects().end() ? tr("Linked components updated.")
            : tr("Linked components updated. Check the mates of %1.").arg(objectName(*failed)));
    } catch (const std::exception& error) {
        QMessageBox::warning(this, title, localizedDiagnostic(error.what()));
    } catch (const Standard_Failure& error) {
        QMessageBox::warning(this, title, localizedDiagnostic(error.GetMessageString()));
    }
}

void MainWindow::editPartProperties()
{
    const auto* object = document_.find(selectedId());
    if (!object || !document::isBody(object->parameters)) {
        return;
    }
    const int id = object->id;
    const auto* linked = std::get_if<document::LinkedComponent>(&object->parameters);
    const auto inherited = linked ? linked->record.properties : document::PartProperties{};
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("componentPropertiesDialog"));
    dialog.setWindowTitle(tr("Component properties"));
    QFormLayout form(&dialog);
    const auto field = [&](const QString& label, const std::string& value, const std::string& fallback) {
        auto* edit = new QLineEdit(QString::fromStdString(value), &dialog);
        edit->setMaxLength(1000);
        edit->setPlaceholderText(QString::fromStdString(fallback));
        form.addRow(label, edit);
        return edit;
    };
    const auto designation = field(tr("Designation"), object->properties.designation, inherited.designation);
    const auto name = field(tr("Name"), object->properties.name, inherited.name);
    const auto material = field(tr("Material"), object->properties.material, inherited.material);
    const auto note = field(tr("Note"), object->properties.note, inherited.note);
    const auto colorName = [](const auto& color) -> std::string {
        if (!color) { return {}; }
        double red, green, blue;
        Quantity_Color((*color)[0], (*color)[1], (*color)[2], Quantity_TOC_RGB)
            .Values(red, green, blue, Quantity_TOC_sRGB);
        return QColor::fromRgbF(red, green, blue).name().toStdString();
    };
    const auto color = field(tr("Color (#RRGGBB)"), colorName(object->properties.color), colorName(inherited.color));
    designation->setObjectName(QStringLiteral("componentDesignation"));
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        document::PartProperties properties{designation->text().trimmed().toStdString(),
            name->text().trimmed().toStdString(), material->text().trimmed().toStdString(), note->text().trimmed().toStdString()};
        const auto colorText = color->text().trimmed();
        if (colorText == QString::fromStdString(colorName(object->properties.color))) {
            properties.color = object->properties.color;
        } else if (!colorText.isEmpty()) {
            const QColor rgb(colorText);
            if (!rgb.isValid() || colorText.size() != 7 || !colorText.startsWith(u'#')) {
                QMessageBox::warning(this, tr("Component properties"), tr("Enter a color as #RRGGBB."));
                return;
            }
            const Quantity_Color linear(rgb.redF(), rgb.greenF(), rgb.blueF(), Quantity_TOC_sRGB);
            properties.color = {{linear.Red(), linear.Green(), linear.Blue()}};
        }
        if (document_.setPartProperties(id, properties)) {
            refreshDocument(id);
        }
    }
}

void MainWindow::showAssemblyInterference()
{
    const auto* object = document_.find(selectedId());
    if (!object || object->status != document::Status::Ready || object->shape.IsNull()) {
        return;
    }
    const auto title = tr("Component interference");
    try {
        const auto problems = geometry::checkInterference(object->shape);
        QStringList lines;
        const auto location = [](const auto& components) {
            QStringList labels;
            for (int part : components) {
                labels << QString::number(part);
            }
            return labels.join(u'.');
        };
        for (std::size_t i = 0; i < std::min<std::size_t>(problems.size(), 100); ++i) {
            const auto& overlap = problems[i];
            lines << tr("Components %1 and %2: %3 mm³")
                .arg(location(overlap.first), location(overlap.second)).arg(overlap.volumeMm3, 0, 'g', 8);
        }
        if (problems.size() > 100) {
            lines << tr("Showing 100 of %1 intersections.").arg(problems.size());
        }
        QMessageBox dialog(QMessageBox::Information, title, problems.empty() ? tr("No overlapping solids.")
            : tr("Overlapping solid pairs: %1").arg(problems.size()), QMessageBox::Ok, this);
        if (!lines.isEmpty()) {
            dialog.setDetailedText(lines.join(u'\n'));
        }
        dialog.exec();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, title, localizedDiagnostic(error.what()));
    } catch (const Standard_Failure& error) {
        QMessageBox::warning(this, title, localizedDiagnostic(error.GetMessageString()));
    }
}

void MainWindow::showAssemblyStructure()
{
    const int id = selectedId();
    if (!document_.find(id)) {
        return;
    }
    const auto record = document_.componentRecord(id);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Assembly structure"));
    dialog.resize(720, 440);
    QVBoxLayout layout(&dialog);
    QTreeWidget tree;
    tree.setHeaderLabels({tr("Component"), tr("Designation"), tr("Material"), tr("Note")});
    layout.addWidget(&tree);
    const auto append = [&](auto&& self, const document::ComponentRecord& item, QTreeWidgetItem* parent,
                            const QString& number) -> void {
        const auto& properties = item.properties;
        auto* row = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(&tree);
        const auto name = properties.name.empty() ? tr("Component") : QString::fromStdString(properties.name);
        row->setText(0, number.isEmpty() ? name : number + QStringLiteral("  ") + name);
        row->setText(1, QString::fromStdString(properties.designation));
        row->setText(2, QString::fromStdString(properties.material));
        row->setText(3, QString::fromStdString(properties.note));
        for (std::size_t i = 0; i < item.children.size(); ++i) {
            const auto index = QString::number(i + 1);
            self(self, item.children[i], row, number.isEmpty() ? index : number + u'.' + index);
        }
    };
    append(append, record, nullptr, {});
    tree.expandAll();
    QDialogButtonBox buttons(QDialogButtonBox::Close);
    layout.addWidget(&buttons);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
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
    if (pendingMachining_ == Machining::MoveFace) {
        endMachining(id);
        bool accepted = false;
        const auto distance = QInputDialog::getDouble(this, tr("Move flat face"), tr("Outward displacement (mm)"),
            5, -100000, 100000, 3, &accepted);
        if (accepted) {
            addFeature(tr("Move flat face"), [=, this] {
                return document_.addAdvanced(document::MoveFace{id, face, distance});
            });
        }
        return;
    }
    if (pendingMachining_ != Machining::Hole) {
        return;
    }
    endMachining(id);
    configureHole(id, face, {10, 10, true});
}

void MainWindow::configureHole(int id, const geometry::FaceReference& face, geometry::HoleSize size, int editing)
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("holeDialog"));
    dialog.setWindowTitle(tr("Hole"));
    auto* form = new QFormLayout(&dialog);
    auto* diameter = new QDoubleSpinBox(&dialog);
    diameter->setObjectName(QStringLiteral("holeDiameter"));
    diameter->setRange(0.01, 100000);
    diameter->setValue(size.diameter);
    diameter->setSuffix(tr(" mm"));
    form->addRow(tr("Diameter"), diameter);
    auto* through = new QCheckBox(tr("Through"), &dialog);
    through->setObjectName(QStringLiteral("holeThrough"));
    through->setChecked(size.through);
    form->addRow(through);
    auto* depth = new QDoubleSpinBox(&dialog);
    depth->setObjectName(QStringLiteral("holeDepth"));
    depth->setRange(0.01, 100000);
    depth->setValue(size.depth);
    depth->setSuffix(tr(" mm"));
    depth->setEnabled(!size.through);
    form->addRow(tr("Blind depth"), depth);
    connect(through, &QCheckBox::toggled, depth, &QDoubleSpinBox::setDisabled);
    auto* style = new QComboBox(&dialog);
    style->addItems({tr("Straight hole"), tr("Counterbore"), tr("Countersink")});
    style->setCurrentIndex(static_cast<int>(size.style));
    form->addRow(tr("Hole type"), style);
    const auto number = [&](const QString& label, double value, double min = 0.01, double max = 100000.) {
        auto* field = new QDoubleSpinBox(&dialog);
        field->setDecimals(3);
        field->setRange(min, max);
        field->setValue(value);
        form->addRow(label, field);
        return field;
    };
    const auto head = number(tr("Head diameter (mm)"), size.headDiameter > 0 ? size.headDiameter : 16);
    const auto recess = number(tr("Counterbore depth (mm)"), size.style == geometry::HoleStyle::Counterbore ? size.seat : 3);
    const auto angle = number(tr("Countersink included angle (°)"),
        size.style == geometry::HoleStyle::Countersink ? size.seat : 90, 30, 150);
    connect(style, &QComboBox::currentIndexChanged, &dialog, [=](int i) {
        head->setEnabled(i != 0); recess->setEnabled(i == 1); angle->setEnabled(i == 2);
    });
    head->setEnabled(style->currentIndex() != 0);
    recess->setEnabled(style->currentIndex() == 1);
    angle->setEnabled(style->currentIndex() == 2);
    auto* threaded = new QCheckBox(tr("Thread designation"), &dialog);
    threaded->setChecked(size.thread.has_value());
    form->addRow(threaded);
    auto* nominal = number(tr("Thread diameter (mm)"), size.thread ? size.thread->diameter : 12);
    auto* pitch = number(tr("Thread pitch (mm)"), size.thread ? size.thread->pitch : 1.75);
    auto* length = number(tr("Thread length (mm)"), size.thread ? size.thread->length : 6);
    auto* left = new QCheckBox(tr("Left-handed thread"), &dialog);
    left->setChecked(size.thread && size.thread->leftHanded);
    form->addRow(left);
    for (auto* widget : std::array<QWidget*, 4>{nominal, pitch, length, left}) {
        widget->setEnabled(threaded->isChecked());
        connect(threaded, &QCheckBox::toggled, widget, &QWidget::setEnabled);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        geometry::HoleSize staged{diameter->value(), depth->value(), through->isChecked()};
        staged.style = static_cast<geometry::HoleStyle>(style->currentIndex());
        if (staged.style != geometry::HoleStyle::Straight) {
            staged.headDiameter = head->value();
            staged.seat = staged.style == geometry::HoleStyle::Counterbore ? recess->value() : angle->value();
        }
        if (threaded->isChecked()) {
            staged.thread = geometry::ThreadSize{nominal->value(), pitch->value(), length->value(), left->isChecked()};
        }
        if (editing) { editFeature(editing, [=, this] { return document_.editHole(editing, staged); }); }
        else { addFeature(tr("Hole"), [=, this] { return document_.addHole(id, face, staged); }); }
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
    configureEdgeTreatment({id, edges.front(), kind, 2, {edges.begin() + 1, edges.end()}});
}

void MainWindow::configureEdgeTreatment(document::EdgeTreatment treatment, int editing)
{
    const bool chamfer = treatment.kind == geometry::EdgeTreatmentKind::Chamfer;
    const auto title = chamfer ? tr("Chamfer") : tr("Fillet");
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("edgeTreatmentDialog"));
    dialog.setWindowTitle(title);
    auto* form = new QFormLayout(&dialog);
    const auto number = [&](double value) {
        auto* field = new QDoubleSpinBox(&dialog);
        field->setDecimals(3);
        field->setRange(.01, 100000);
        field->setValue(value);
        return field;
    };
    auto* size = number(treatment.size);
    size->setObjectName(QStringLiteral("edgeStartRadius"));
    form->addRow(chamfer ? tr("Equal distances (mm)") : tr("Radius (mm)"), size);
    auto* tangency = new QCheckBox(tr("Follow tangent chain"), &dialog);
    tangency->setChecked(treatment.followTangency);
    form->addRow(tangency);
    auto* variable = new QCheckBox(tr("Variable radius"), &dialog);
    auto* end = number(treatment.endRadius.value_or(1));
    variable->setChecked(treatment.endRadius.has_value());
    end->setEnabled(variable->isChecked());
    if (!chamfer) {
        form->addRow(variable);
        form->addRow(tr("End radius (mm)"), end);
        connect(variable, &QCheckBox::toggled, end, &QWidget::setEnabled);
    } else { variable->hide(); end->hide(); }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        treatment.size = size->value();
        treatment.followTangency = tangency->isChecked();
        treatment.endRadius = variable->isChecked() ? std::optional(end->value()) : std::nullopt;
        if (editing) { editFeature(editing, [&, this] { return document_.editEdgeTreatment(editing, treatment); }); }
        else { addFeature(title, [&, this] { return document_.addEdgeTreatment(treatment); }); }
    } else {
        statusBar()->showMessage(tr("%1 cancelled").arg(title));
    }
}

void MainWindow::configureAdvanced(const QString& title, const document::Parameters& defaults, int editing)
{
    if (pages_->currentWidget() != viewport_ || pendingMachining_) {
        return;
    }
    if (std::holds_alternative<geometry::SpatialCurve>(defaults)) { configureSpatialCurve(editing); return; }
    if (const auto* surface = std::get_if<document::Surface>(&defaults)) { configureSurface(surface->kind, editing); return; }
    if (const auto* annotation = std::get_if<document::Annotation3d>(&defaults)) {
        configureAnnotation(annotation->kind, editing);
        return;
    }
    try {
        const auto result = advancedParameters(this, title, document_, defaults, editing);
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

void MainWindow::configureFeaturePattern(int seed)
{
    const auto* object = document_.find(seed);
    if (!object || !document::featureBase(object->parameters)) {
        report(tr("Select an individual operation in the model tree."));
        return;
    }
    configureAdvanced(tr("Operation pattern"), document::FeaturePattern{seed, 4, {20, 0, 0}});
}

void MainWindow::measureBody()
{
    const auto* body = document_.find(selectedId());
    if (!body || body->status != document::Status::Ready || !document::isBody(body->parameters)) {
        return;
    }
    const int id = body->id;
    try {
        if (const auto value = bodyPropertiesDialog(this, document_, *body)) {
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
