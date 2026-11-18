#include "gui/DraftWindow.h"
#include "gui/DocumentWindow.h"
#include "io/DxfIO.h"

#include "gui/DraftCanvas.h"
#include "gui/InterfaceLanguage.h"
#include "gui/LineInput.h"
#include "gui/UserPreferences.h"
#include "io/DraftIO.h"
#include "io/ModelViewsIO.h"

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QGridLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabBar>
#include <QToolBar>
#include <QUndoCommand>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <array>
#include <exception>
#include <functional>

namespace graph3d {
namespace {

using drawing::DraftItem;
using drawing::DraftKind;
using drawing::DraftStroke;

class DraftEdit final : public QUndoCommand {
public:
    DraftEdit(const QString& text, std::function<void()> redo, std::function<void()> undo)
        : QUndoCommand(text), redo_(std::move(redo)), undo_(std::move(undo)) {}
    void redo() override { redo_(); }
    void undo() override { undo_(); }

private:
    std::function<void()> redo_, undo_;
};

QString chooseSavePath(QWidget* parent, const QString& title, const QString& path,
                       const QString& filter, const QString& suffix)
{
    QFileDialog dialog(parent, title, path, filter);
    dialog.setOption(QFileDialog::DontUseNativeDialog, darkThemeEnabled() || russianInterface());
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(suffix);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return {};
    }
    const auto selected = dialog.selectedFiles().constFirst();
    if (!selected.endsWith(QStringLiteral(".") + suffix, Qt::CaseInsensitive)) {
        QMessageBox::warning(parent, title, QObject::tr("Use the .%1 extension.").arg(suffix));
        return {};
    }
    return selected;
}

}  // namespace

DraftWindow::DraftWindow(QWidget* parent, DocumentWindow* host)
    : QMainWindow(parent, host ? Qt::Widget : Qt::Window), host_(host), sourceWatch_(new QFileSystemWatcher(this)),
      modelStatus_(new QLabel(this))
{
    setObjectName(QStringLiteral("draftWindow"));
    setAttribute(Qt::WA_DeleteOnClose, !host_);
    resize(1100, 760);
    auto* workspace = new QWidget(this);
    auto* layout = new QVBoxLayout(workspace);
    layout->setContentsMargins(8, 0, 8, 0);
    auto* lineInput = new LineInput(workspace);
    canvas_ = new DraftCanvas(draft_, *lineInput, workspace);
    sheetTabs_ = new QTabBar(workspace);
    sheetTabs_->setObjectName(QStringLiteral("draftSheetTabs"));
    sheetTabs_->setExpanding(false);
    layout->addWidget(sheetTabs_);
    connect(sheetTabs_, &QTabBar::currentChanged, this, &DraftWindow::activateSheet);
    updateSheetTabs();
    layout->addWidget(lineInput);
    layout->addWidget(canvas_, 1);
    setCentralWidget(workspace);
    connect(canvas_, &DraftCanvas::lineOutsideSheet, this, [this] {
        statusBar()->showMessage(tr("The line endpoint must be inside the drawing area."), 4000);
    });
    auto* file = menuBar()->addMenu(tr("&File"));
    if (host_) {
        file->addAction(tr("Start page"), this, [this] { host_->showHome(); });
    }
    file->addAction(tr("New drawing"), QKeySequence::New, this, [this] {
        if (host_) {
            host_->newDrawing();
        } else {
            newDrawing();
        }
    });
    file->addAction(tr("Open drawing…"), QKeySequence::Open, this, &DraftWindow::browseDrawing);
    auto* save = file->addAction(tr("Save"), QKeySequence::Save, this, [this] { saveDrawing(false); });
    save->setObjectName(QStringLiteral("saveDraft"));
    file->addAction(tr("Save as…"), QKeySequence::SaveAs, this, [this] { saveDrawing(true); });
    auto* pdf = file->addAction(tr("Export PDF…"), this, &DraftWindow::exportPdf);
    pdf->setObjectName(QStringLiteral("exportDraftPdf"));
    file->addAction(tr("Import DXF…"), this, &DraftWindow::browseDxf)->setObjectName(QStringLiteral("importDraftDxf"));
    file->addAction(tr("Export current sheet to DXF…"), this, &DraftWindow::exportDxf)->setObjectName(QStringLiteral("exportDraftDxf"));
    file->addSeparator();
    if (host_) {
        file->addAction(tr("Close drawing"), this, [this] { host_->closeCurrent(); });
    } else {
        file->addAction(tr("Close drawing"), QKeySequence::Close, this, &QWidget::close);
    }

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    auto* undo = history_.createUndoAction(this, tr("Undo"));
    undo->setObjectName(QStringLiteral("undoDraft"));
    undo->setShortcuts(QKeySequence::Undo);
    edit->addAction(undo);
    auto* redo = history_.createRedoAction(this, tr("Redo"));
    redo->setObjectName(QStringLiteral("redoDraft"));
    redo->setShortcuts(QKeySequence::Redo);
    edit->addAction(redo);
    auto* remove = edit->addAction(tr("Delete selected"), QKeySequence::Delete, this, [this] {
        if (const int index = canvas_->selected(); index >= 0) {
            removeItem(index);
        }
    });
    auto* properties = edit->addAction(tr("Item properties…"), this, &DraftWindow::editSelected);
    properties->setObjectName(QStringLiteral("draftItemProperties"));
    auto* reattach = edit->addAction(tr("Reattach dimension to view…"), this, &DraftWindow::reattachDimension);
    reattach->setObjectName(QStringLiteral("reattachDraftDimension"));
    reattach->setEnabled(false);
    remove->setObjectName(QStringLiteral("removeDraftItem"));
    remove->setEnabled(false);
    properties->setEnabled(false);
    connect(canvas_, &DraftCanvas::selectionChanged, this, [this, remove, properties, reattach](int selected) {
        remove->setEnabled(selected >= 0);
        reattach->setEnabled(selected >= 0 && drawing::isDimension(draft_.items[selected].kind));
        properties->setEnabled(selected >= 0 && draft_.items[selected].kind != DraftKind::Polyline
                               && draft_.items[selected].kind != DraftKind::Hatch
                               && draft_.items[selected].kind != DraftKind::Spline);
    });
    edit->addAction(tr("Cancel tool"), Qt::Key_Escape, canvas_, &DraftCanvas::cancel);
    auto* sheet = menuBar()->addMenu(tr("&Sheet"));
    auto* settings = sheet->addAction(tr("Format and title block…"), this, &DraftWindow::editSheet);
    settings->setObjectName(QStringLiteral("draftSheetSettings"));
    sheet->addAction(tr("Add sheet"), this, [this] { changeSheetCount(true); })->setObjectName(QStringLiteral("addDraftSheet"));
    sheet->addAction(tr("Remove sheet"), this, [this] { changeSheetCount(false); })->setObjectName(QStringLiteral("removeDraftSheet"));
    sheet->addAction(tr("Technical requirements…"), this, [this] {
        bool accepted = false;
        const auto text = QInputDialog::getMultiLineText(this, tr("Technical requirements"),
            tr("Enter each requirement on a separate line."), draft_.paper.requirements, &accepted);
        if (accepted && text.size() > 2400) {
            QMessageBox::warning(this, tr("Technical requirements"), tr("Use at most 2400 characters."));
        } else if (accepted && text != draft_.paper.requirements) {
            pushSheetEdit(tr("Edit technical requirements"),
                [this, text] { draft_.paper.requirements = text; },
                [this, before = draft_.paper.requirements] { draft_.paper.requirements = before; });
        }
    });
    auto* fit = sheet->addAction(tr("Fit sheet"), Qt::Key_F, canvas_, &DraftCanvas::fit);
    sheet->addAction(tr("Edit custom title block…"), this, &DraftWindow::editTitleBlock)
        ->setObjectName(QStringLiteral("draftCustomTitleBlock"));
    sheet->addAction(tr("Save drawing template…"), this, &DraftWindow::saveDrawingTemplate)
        ->setObjectName(QStringLiteral("saveDraftTemplate"));
    sheet->addAction(tr("New from drawing template…"), this, &DraftWindow::newFromDrawingTemplate)
        ->setObjectName(QStringLiteral("openDraftTemplate"));
    auto* grid = sheet->addAction(tr("Show grid"));
    grid->setCheckable(true);
    grid->setChecked(true);
    connect(grid, &QAction::toggled, canvas_, &DraftCanvas::setGrid);

    auto* views = menuBar()->addMenu(tr("Model &views"));
    auto* insert = views->addAction(tr("Insert from model or change source…"), this, &DraftWindow::chooseModel);
    insert->setObjectName(QStringLiteral("linkedViewsSource"));
    modelRefresh_ = views->addAction(tr("Update from model"), this, &DraftWindow::refreshModel);
    modelRefresh_->setObjectName(QStringLiteral("linkedViewsRefresh"));
    modelSettings_ = views->addAction(tr("View layout and section…"), this, &DraftWindow::editModelViews);
    modelSettings_->setObjectName(QStringLiteral("linkedViewsSettings"));
    views->addAction(tr("Arrange and add views…"), this, &DraftWindow::manageDrawingViews)
        ->setObjectName(QStringLiteral("manageDrawingViews"));
    sectionLine_ = views->addAction(tr("Place section line on view"), this, &DraftWindow::placeSectionLine);
    localRegion_ = views->addAction(tr("Place local section area"), this, &DraftWindow::placeLocalSection);
    modelDetail_ = views->addAction(tr("Detail view…"), this, &DraftWindow::editDetail);
    modelDetail_->setObjectName(QStringLiteral("linkedDetail"));
    sectionLine_->setObjectName(QStringLiteral("placeSectionLine"));
    modelRemove_ = views->addAction(tr("Remove model views"), this, [this] {
        setModelViews({}, tr("Remove model views"));
    });
    auto* assembly = menuBar()->addMenu(tr("Assembly &drawing"));
    specification_ = assembly->addAction(tr("View specification…"), this, &DraftWindow::showSpecification);
    specification_->setObjectName(QStringLiteral("viewSpecification"));
    placePosition_ = assembly->addAction(tr("Place position number…"), this, &DraftWindow::placePosition);
    placePosition_->setObjectName(QStringLiteral("placeAssemblyPosition"));
    removePosition_ = assembly->addAction(tr("Remove position number…"), this, &DraftWindow::removePosition);
    specificationExport_ = assembly->addAction(tr("Include specification in PDF"));
    specificationExport_->setCheckable(true);
    connect(specificationExport_, &QAction::toggled, this, &DraftWindow::setSpecificationExport);
    modelStatus_->setObjectName(QStringLiteral("linkedViewsStatus"));
    modelStatus_->setTextFormat(Qt::PlainText);
    modelStatus_->setWordWrap(true);
    layout->addWidget(modelStatus_);
    connect(sourceWatch_, &QFileSystemWatcher::fileChanged, this, [this] { updateModelStatus(); });
    updateModelStatus();

    auto* bar = addToolBar(tr("Drawing tools"));
    bar->setMovable(false);
    bar->setFloatable(false);
    auto* group = new QActionGroup(this);
    const auto tool = [&](const QString& name, const char* objectName, std::optional<DraftKind> kind, const QString& hint, QMenu* menu = nullptr) {
        auto* action = menu ? menu->addAction(name) : bar->addAction(name);
        action->setObjectName(QString::fromLatin1(objectName));
        action->setCheckable(true);
        action->setToolTip(hint);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, kind, hint] {
            if (kind && (drawing::isLinearDimension(*kind) || *kind == DraftKind::Radius || *kind == DraftKind::Diameter)) {
                prepareModelDimensions();
            }
            canvas_->setTool(kind);
            statusBar()->showMessage(hint);
        });
        return action;
    };
    auto* select = tool(tr("Select"), "draftSelect", {}, tr("Click an item to select it. Drag to move it."));
    tool(tr("Line"), "draftLine", DraftKind::Line, tr("Pick the start, then click the endpoint or enter a length and angle."));
    tool(tr("Rectangle"), "draftRect", DraftKind::Rectangle, tr("Pick two opposite corners."));
    tool(tr("Circle"), "draftCircle", DraftKind::Circle, tr("Pick the center and a point on the circle."));
    tool(tr("Text"), "draftText", DraftKind::Text, tr("Pick a position for a note."));
    bar->addSeparator();
    tool(tr("Horizontal dim."), "draftHorizontal", DraftKind::Horizontal, tr("Pick two points, then place the dimension line. Snapped points stay attached."));
    tool(tr("Vertical dim."), "draftVertical", DraftKind::Vertical, tr("Pick two points, then place the dimension line. Snapped points stay attached."));
    auto* geometryTools = menuBar()->addMenu(tr("&Geometry"));
    tool(tr("Three-point arc"), "draftArc", DraftKind::Arc,
         tr("Pick the start, a point on the arc, and the end."), geometryTools);
    tool(tr("Polyline"), "draftPolyline", DraftKind::Polyline,
         tr("Pick vertices. Click the start to close, or press Enter to finish."), geometryTools);
    tool(tr("Hatch"), "draftHatch", DraftKind::Hatch,
         tr("Click inside a closed contour to hatch it."), geometryTools);
    tool(tr("Ellipse"), "draftEllipse", DraftKind::Ellipse, tr("Pick opposite corners of the ellipse box."), geometryTools);
    tool(tr("Spline"), "draftSpline", DraftKind::Spline,
         tr("Pick points on the spline. Click the start to close, or press Enter to finish."), geometryTools);
    tool(tr("Construction line"), "draftConstruction", DraftKind::ConstructionLine,
         tr("Pick two points to define an auxiliary line."), geometryTools);
    geometryTools->addAction(tr("Save visible objects as fragment…"), this, &DraftWindow::saveFragment)
        ->setObjectName(QStringLiteral("saveDraftFragment"));
    geometryTools->addAction(tr("Insert fragment…"), this, &DraftWindow::insertFragment)
        ->setObjectName(QStringLiteral("insertDraftFragment"));
    auto* modify = menuBar()->addMenu(tr("&Modify"));
    const auto modification = [&](const QString& name, const char* id, DraftModify mode, const QString& hint) {
        auto* action = modify->addAction(name);
        action->setObjectName(QString::fromLatin1(id));
        action->setCheckable(true);
        action->setToolTip(hint);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode, hint] {
            canvas_->setModify(mode);
            statusBar()->showMessage(hint);
        });
    };
    modification(tr("Trim"), "draftTrim", DraftModify::Trim, tr("Click the part of a curve to remove."));
    modification(tr("Extend"), "draftExtend", DraftModify::Extend, tr("Click near the end to extend to another curve."));
    modification(tr("Parallel offset"), "draftOffset", DraftModify::Offset,
                 tr("Click an object to create an offset copy."));
    auto* dimensions = menuBar()->addMenu(tr("D&imensions"));
    tool(tr("Aligned dimension"), "draftAligned", DraftKind::Aligned,
         tr("Pick two points, then place the dimension line."), dimensions);
    tool(tr("Angular dimension"), "draftAngular", DraftKind::Angular,
         tr("Pick the vertex, one point on each arm, and the label position."), dimensions);
    tool(tr("Radius dimension"), "draftRadius", DraftKind::Radius,
         tr("Click a circle or arc, then place the dimension label."), dimensions);
    tool(tr("Diameter dimension"), "draftDiameter", DraftKind::Diameter,
         tr("Click a circle or arc, then place the dimension label."), dimensions);
    auto* annotations = menuBar()->addMenu(tr("&Annotations"));
    const auto hint = tr("Pick the feature, then place the label.");
    tool(tr("Leader"), "draftLeader", DraftKind::Leader, hint, annotations);
    tool(tr("Datum"), "draftDatum", DraftKind::Datum, hint, annotations);
    tool(tr("Geometric tolerance"), "draftTolerance", DraftKind::Tolerance, hint, annotations);
    tool(tr("Surface roughness"), "draftRoughness", DraftKind::Roughness, hint, annotations);
    tool(tr("Thread designation"), "draftThread", DraftKind::Thread, hint, annotations);
    tool(tr("Weld designation"), "draftWeld", DraftKind::Weld, hint, annotations);
    annotations->addAction(tr("Insert table…"), this, [this] { editTable(); })
        ->setObjectName(QStringLiteral("insertDraftTable"));
    auto* layerMenu = menuBar()->addMenu(tr("&Layers"));
    layerMenu->addAction(tr("Manage layers…"), this, &DraftWindow::editLayers);
    layerMenu->addAction(tr("Move selection to active layer"), this, [this] {
        const int index = canvas_->selected();
        const int layer = layers_->currentIndex();
        if (index >= 0 && drawing::layerEditable(draft_, layer) && draft_.items[index].layer != layer) {
            auto item = draft_.items[index];
            item.layer = layer;
            editItem(index, item, tr("Move item to layer"));
        }
    });
    layers_ = new QComboBox(bar);
    layers_->setObjectName(QStringLiteral("draftActiveLayer"));
    layers_->setAccessibleName(tr("Active layer"));
    bar->addWidget(layers_);
    updateLayers();
    auto* stroke = new QComboBox(bar);
    stroke->setAccessibleName(tr("Line style"));
    stroke->addItems({tr("Solid"), tr("Hidden"), tr("Center")});
    bar->addWidget(stroke);
    connect(stroke, &QComboBox::currentIndexChanged, this, [this](int value) {
        const auto line = static_cast<DraftStroke>(value);
        canvas_->setStroke(line);
        if (const int index = canvas_->selected(); index >= 0) {
            auto item = draft_.items[index];
            if (!drawing::draftAnchors(item.kind).empty()) {
                item.stroke = line;
                if (item != draft_.items[index]) {
                    editItem(index, item, tr("Change line style"));
                }
            }
        }
    });
    bar->addSeparator();
    properties->setIconText(tr("Properties"));
    settings->setIconText(tr("Sheet"));
    pdf->setIconText(tr("PDF"));
    bar->addAction(properties);
    bar->addAction(settings);
    bar->addAction(fit);
    bar->addAction(pdf);
    auto* coordinates = new QLabel(this);
    statusBar()->addPermanentWidget(coordinates);
    connect(canvas_, &DraftCanvas::positionChanged, this, [coordinates](QPointF point) {
        coordinates->setText(tr("X %1   Y %2 mm").arg(point.x(), 0, 'f', 1).arg(point.y(), 0, 'f', 1));
    });
    connect(canvas_, &DraftCanvas::itemCreated, this, [this](const DraftItem& item) {
        auto identified = item;
        identified.layer = layers_->currentIndex();
        if (!drawing::layerEditable(draft_, identified.layer)) {
            statusBar()->showMessage(tr("Select a visible, unlocked layer to draw."), 4000);
            return;
        }
        identified.id = QUuid::createUuid();
        editItem(static_cast<int>(draft_.items.size()), identified, tr("Create item"));
    });
    connect(canvas_, &DraftCanvas::itemMoved, this, [this](int index, const DraftItem& item) {
        replaceCurve(index, {item}, tr("Move item"));
    });
    connect(canvas_, &DraftCanvas::changeRequested, this, [this](int index, const auto& items) {
        replaceCurve(index, items, tr("Modify curve"));
    });
    connect(canvas_, &DraftCanvas::toolMessage, this, [this](const QString& text) {
        statusBar()->showMessage(text);
    });
    connect(&history_, &QUndoStack::indexChanged, this, [this] {
        canvas_->cancel();
        canvas_->update();
        updateTitle();
    });
    connect(&history_, &QUndoStack::cleanChanged, this, [this] { updateTitle(); });
    select->trigger();
    updateTitle();
}

void DraftWindow::removeItem(int index)
{
    history_.beginMacro(tr("Delete item"));
    detachLostPoints(index, nullptr);
    editItem(index, {}, tr("Delete item"));
    history_.endMacro();
}

void DraftWindow::replaceCurve(int index, const std::vector<DraftItem>& items, const QString& description)
{
    history_.beginMacro(description);
    detachLostPoints(index, &items.front());
    editItem(index, items.front(), description);
    for (std::size_t i = 1; i < items.size(); ++i) {
        editItem(static_cast<int>(draft_.items.size()), items[i], description);
    }
    history_.endMacro();
}

void DraftWindow::detachLostPoints(int index, const DraftItem* replacement)
{
    const auto id = draft_.items[index].id;
    if (drawing::draftAnchors(draft_.items[index].kind).empty()) {
        return;
    }
    const drawing::DraftGeometry geometry(draft_);
    const drawing::DraftGeometry changed(draft_, replacement);
    const auto detach = [&](const auto& anchor) {
        return anchor && (anchor->item == id || anchor->crossing == id)
            && (!replacement || !changed.validReference(*anchor)
                || (anchor->anchor == drawing::DraftAnchor::Intersection
                    && geometry.point(*anchor) && !changed.point(*anchor)));
    };
    for (int i = 0; i < static_cast<int>(draft_.items.size()); ++i) {
        const auto& dimension = draft_.items[i];
        const bool detachA = detach(dimension.anchorA);
        const bool detachB = detach(dimension.anchorB);
        const bool detachC = detach(dimension.anchorC);
        if (detachA || detachB || detachC) {
            auto fixed = geometry.resolve(dimension);
            if (detachA) {
                fixed.anchorA.reset();
            }
            if (detachB) {
                fixed.anchorB.reset();
            }
            if (detachC) {
                fixed.anchorC.reset();
            }
            editItem(i, fixed, tr("Detach dimension point"));
        }
    }
}

void DraftWindow::editItem(int index, const std::optional<DraftItem>& after, const QString& description)
{
    const auto before = index < static_cast<int>(draft_.items.size()) ? std::optional(draft_.items[index]) : std::nullopt;
    const auto apply = [this, index](bool exists, const std::optional<DraftItem>& item) {
        auto position = draft_.items.begin() + index;
        if (!item) {
            canvas_->select(-1);
            draft_.items.erase(position);
        } else if (exists) {
            *position = *item;
        } else {
            canvas_->select(-1);
            draft_.items.insert(position, *item);
        }
    };
    pushSheetEdit(description, [=] { apply(before.has_value(), after); },
                  [=] { apply(after.has_value(), before); });
}

DraftWindow::~DraftWindow()
{
    disconnect(&history_, nullptr, this, nullptr);
}

void DraftWindow::replaceDrawing(drawing::Draft draft, const QString& path)
{
    canvas_->cancel();
    canvas_->select(-1);
    sheets_.assign(1, drawing::Draft{});
    for (auto& sheet : draft.additionalSheets) {
        sheets_.push_back(std::move(sheet));
    }
    draft.additionalSheets.clear();
    activeSheet_ = 0;
    draft_ = std::move(draft);
    updateSheetTabs();
    updateLayers();
    path_ = path;
    history_.clear();
    canvas_->fit();
    updateModelStatus();
    updateTitle();
}

void DraftWindow::pushSheetEdit(const QString& name, std::function<void()> redo, std::function<void()> undo)
{
    const int sheet = activeSheet_;
    history_.push(new DraftEdit(name, [this, sheet, redo = std::move(redo)] {
        activateSheet(sheet);
        redo();
    }, [this, sheet, undo = std::move(undo)] {
        activateSheet(sheet);
        undo();
    }));
}

drawing::Draft DraftWindow::wholeDrawing() const
{
    auto document = activeSheet_ == 0 ? draft_ : sheets_.front();
    for (int i = 1; i < static_cast<int>(sheets_.size()); ++i) {
        document.additionalSheets.push_back(i == activeSheet_ ? draft_ : sheets_[i]);
    }
    return document;
}

void DraftWindow::updateSheetTabs()
{
    const QSignalBlocker blocker(sheetTabs_);
    while (sheetTabs_->count() < static_cast<int>(sheets_.size())) {
        sheetTabs_->addTab(tr("Sheet %1").arg(sheetTabs_->count() + 1));
    }
    while (sheetTabs_->count() > static_cast<int>(sheets_.size())) {
        sheetTabs_->removeTab(sheetTabs_->count() - 1);
    }
    sheetTabs_->setCurrentIndex(activeSheet_);
    canvas_->setPageNumber(activeSheet_ + 1, static_cast<int>(sheets_.size()));
}

void DraftWindow::activateSheet(int index)
{
    if (index < 0 || index >= static_cast<int>(sheets_.size()) || index == activeSheet_) {
        return;
    }
    canvas_->cancel();
    canvas_->select(-1);
    std::swap(draft_, sheets_[activeSheet_]);
    activeSheet_ = index;
    std::swap(draft_, sheets_[activeSheet_]);
    updateSheetTabs();
    layers_->setCurrentIndex(0);
    updateLayers();
    canvas_->fit();
    updateModelStatus();
}

void DraftWindow::changeSheetCount(bool add)
{
    const int size = static_cast<int>(sheets_.size());
    if (add && size >= drawing::maxSheets) {
        statusBar()->showMessage(tr("The drawing already has the maximum number of sheets."), 4000);
        return;
    }
    if (!add && size == 1) {
        statusBar()->showMessage(tr("A drawing must have at least one sheet."), 4000);
        return;
    }
    const int index = add ? size : activeSheet_;
    const int previous = activeSheet_;
    drawing::Draft sheet;
    if (add) {
        sheet.a3 = draft_.a3;
        sheet.title = draft_.title;
        sheet.number = draft_.number;
        sheet.author = draft_.author;
        sheet.paper = draft_.paper;
        sheet.paper.requirements.clear();
        sheet.layers = draft_.layers;
    } else {
        sheet = draft_;
    }
    const auto insert = [this, index, sheet] {
        sheets_.insert(sheets_.begin() + index, sheet);
        if (activeSheet_ >= index) {
            ++activeSheet_;
        }
        activateSheet(index);
    };
    const auto erase = [this, index, previous] {
        const int target = index == previous ? (index == 0 ? 1 : index - 1) : previous;
        activateSheet(target);
        sheets_.erase(sheets_.begin() + index);
        if (activeSheet_ > index) {
            --activeSheet_;
        }
        updateSheetTabs();
    };
    history_.push(new DraftEdit(add ? tr("Add sheet") : tr("Remove sheet"),
        add ? std::function<void()>(insert) : erase, add ? std::function<void()>(erase) : insert));
}

void DraftWindow::updateLayers()
{
    const int index = layers_->currentIndex();
    layers_->clear();
    for (const auto& layer : draft_.layers) {
        auto name = layer.name.isEmpty() ? tr("Default layer") : layer.name;
        if (!layer.visible) {
            name += tr(" (hidden)");
        }
        if (layer.locked) {
            name += tr(" (locked)");
        }
        layers_->addItem(name);
    }
    layers_->setCurrentIndex(std::clamp(index, 0, layers_->count() - 1));
}

void DraftWindow::editLayers()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Manage layers"));
    auto* form = new QFormLayout(&dialog);
    auto* list = new QComboBox(&dialog);
    auto* name = new QLineEdit(&dialog);
    name->setMaxLength(200);
    auto* visible = new QCheckBox(tr("Visible"), &dialog);
    auto* locked = new QCheckBox(tr("Locked"), &dialog);
    auto* add = new QPushButton(tr("Add layer"), &dialog);
    auto* removeLayer = new QPushButton(tr("Remove empty layer"), &dialog);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(tr("Layer"), list);
    form->addRow(tr("Name"), name);
    form->addRow(visible, locked);
    form->addRow(add, removeLayer);
    form->addRow(buttons);
    auto edited = draft_.layers;
    std::vector<int> originalIndices;
    for (int i = 0; i < static_cast<int>(edited.size()); ++i) {
        originalIndices.push_back(i);
    }
    const auto caption = [this](const drawing::DraftLayer& layer) {
        return layer.name.isEmpty() ? tr("Default layer") : layer.name;
    };
    for (const auto& layer : edited) {
        list->addItem(caption(layer));
    }
    list->setCurrentIndex(layers_->currentIndex());
    const auto showLayer = [&](int index) {
        name->setText(edited[index].name);
        visible->setChecked(edited[index].visible);
        locked->setChecked(edited[index].locked);
        const int original = originalIndices[index];
        removeLayer->setEnabled(index > 0 && (original < 0
            || std::ranges::none_of(draft_.items, [original](const auto& item) { return item.layer == original; })));
    };
    showLayer(list->currentIndex());
    connect(list, &QComboBox::currentIndexChanged, &dialog, showLayer);
    connect(name, &QLineEdit::textEdited, &dialog, [&](const QString& text) {
        auto& layer = edited[list->currentIndex()];
        layer.name = text.trimmed();
        list->setItemText(list->currentIndex(), caption(layer));
    });
    connect(visible, &QCheckBox::clicked, &dialog, [&](bool checked) { edited[list->currentIndex()].visible = checked; });
    connect(locked, &QCheckBox::clicked, &dialog, [&](bool checked) { edited[list->currentIndex()].locked = checked; });
    connect(add, &QPushButton::clicked, &dialog, [&] {
        if (edited.size() < drawing::maxLayers) {
            edited.push_back({tr("Layer %1").arg(edited.size() + 1)});
            originalIndices.push_back(-1);
            list->addItem(caption(edited.back()));
            list->setCurrentIndex(list->count() - 1);
        }
        add->setEnabled(edited.size() < drawing::maxLayers);
    });
    add->setEnabled(edited.size() < drawing::maxLayers);
    connect(removeLayer, &QPushButton::clicked, &dialog, [&] {
        const int index = list->currentIndex();
        edited.erase(edited.begin() + index);
        originalIndices.erase(originalIndices.begin() + index);
        list->removeItem(index);
        showLayer(list->currentIndex());
        add->setEnabled(true);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        std::vector<int> beforeLayers, afterLayers;
        for (const auto& item : draft_.items) {
            beforeLayers.push_back(item.layer);
            const auto it = std::ranges::find(originalIndices, item.layer);
            afterLayers.push_back(static_cast<int>(it - originalIndices.begin()));
        }
        if (edited != draft_.layers || beforeLayers != afterLayers) {
            const auto apply = [this](const auto& layers, const auto& itemLayers) {
                for (std::size_t i = 0; i < itemLayers.size(); ++i) {
                    draft_.items[i].layer = itemLayers[i];
                }
                canvas_->select(-1);
                draft_.layers = layers;
                updateLayers();
            };
            pushSheetEdit(tr("Edit layers"), [apply, edited, afterLayers] { apply(edited, afterLayers); },
                          [apply, before = draft_.layers, beforeLayers] { apply(before, beforeLayers); });
        }
        layers_->setCurrentIndex(list->currentIndex());
    }
}

void DraftWindow::updateTitle()
{
    setWindowTitle(tr("%1[*] — Graph3D Drawing").arg(path_.isEmpty() ? tr("Untitled") : QFileInfo(path_).fileName()));
    setWindowModified(!history_.isClean());
    if (host_) {
        host_->documentChanged(this);
    }
}

bool DraftWindow::newDrawing()
{
    if (!confirmSave()) {
        return false;
    }
    replaceDrawing({});
    return true;
}

bool DraftWindow::openDrawing(const QString& path)
{
    if (!path_.isEmpty() && QFileInfo(path) == QFileInfo(path_)) {
        return true;
    }
    try {
        auto draft = io::openDraft(path);
        if (!confirmSave()) {
            return false;
        }
        replaceDrawing(std::move(draft), path);
        emit fileUsed(path);
        return true;
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Open drawing"), localizedDiagnostic(error.what()));
        return false;
    }
}

void DraftWindow::browseDrawing()
{
    if (host_) {
        host_->browse();
        return;
    }
    const auto path = QFileDialog::getOpenFileName(this, tr("Open drawing"), path_, tr("Graph3D drawings (*.g3draw)"),
        nullptr, (darkThemeEnabled() || russianInterface()) ? QFileDialog::DontUseNativeDialog : QFileDialog::Options{});
    if (!path.isEmpty()) {
        openDrawing(path);
    }
}

bool DraftWindow::saveDrawing(bool saveAs)
{
    const auto path = saveAs || path_.isEmpty()
        ? chooseSavePath(this, tr("Save drawing"), path_, tr("Graph3D drawings (*.g3draw)"), QStringLiteral("g3draw"))
        : path_;
    if (path.isEmpty()) {
        return false;
    }
    if (host_ && !host_->canSaveAs(this, path)) {
        return false;
    }
    try {
        io::saveDraft(path, wholeDrawing());
        path_ = path;
        history_.setClean();
        updateTitle();
        emit fileUsed(path);
        return true;
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Save drawing"), localizedDiagnostic(error.what()));
        return false;
    }
}

void DraftWindow::browseDxf()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Import DXF"), path_, tr("DXF files (*.dxf)"),
        nullptr, (darkThemeEnabled() || russianInterface()) ? QFileDialog::DontUseNativeDialog : QFileDialog::Options{});
    if (!path.isEmpty()) {
        if (host_) {
            host_->openDocument(path);
        } else {
            importDxf(path);
        }
    }
}

bool DraftWindow::importDxf(const QString& path)
{
    try {
        auto loaded = io::importDxf(path);
        if (!confirmSave()) {
            return false;
        }
        replaceDrawing(std::move(loaded.drawing));
        history_.resetClean();
        QString message = tr("Imported %1").arg(QFileInfo(path).fileName());
        if (loaded.skipped) {
            message += tr(". Unsupported entities skipped: %1.").arg(loaded.skipped);
        }
        if (loaded.beyondSheet) {
            message += tr(" Geometry exceeds the sheet at 1:1.");
        }
        statusBar()->showMessage(message);
        return true;
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Import DXF"), localizedDiagnostic(error.what()));
        return false;
    }
}

void DraftWindow::exportDxf()
{
    const auto path = chooseSavePath(this, tr("Export current sheet to DXF"), {},
                                     tr("DXF files (*.dxf)"), QStringLiteral("dxf"));
    if (path.isEmpty()) {
        return;
    }
    try {
        const int skipped = io::exportDxf(path, draft_);
        auto message = tr("Exported %1").arg(QFileInfo(path).fileName());
        if (skipped) {
            message += tr(". Unsupported entities skipped: %1.").arg(skipped);
        }
        statusBar()->showMessage(message);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Export current sheet to DXF"), localizedDiagnostic(error.what()));
    }
}

void DraftWindow::exportPdf()
{
    const auto path = chooseSavePath(this, tr("Export drawing PDF"), {}, tr("PDF files (*.pdf)"), QStringLiteral("pdf"));
    if (path.isEmpty()) {
        return;
    }
    try {
        drawing::exportDraftPdf(path, wholeDrawing());
        statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()));
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Export drawing PDF"), localizedDiagnostic(error.what()));
    }
}

void DraftWindow::chooseModel()
{
    const auto* current = draft_.modelViews ? &*draft_.modelViews : nullptr;
    const auto path = QFileDialog::getOpenFileName(this, tr("Select the source model"),
        current ? current->sourcePath : path_, tr("Graph3D projects (*.graph3d)"), nullptr,
        darkThemeEnabled() || russianInterface() ? QFileDialog::DontUseNativeDialog : QFileDialog::Options{});
    if (path.isEmpty()) {
        return;
    }
    try {
        auto source = io::openModelSource(path);
        std::optional<QString> variant;
        if (!source.document.configuration().variants.empty()) {
            QStringList choices{tr("Use the model's active variant"), tr("Base values")};
            std::vector<std::optional<QString>> values{std::nullopt, QString{}};
            for (const auto& [name, overrides] : source.document.configuration().variants) {
                const auto label = QString::fromStdString(name);
                choices.append(tr("Variant: %1").arg(label));
                values.emplace_back(label);
            }
            int index = 0;
            if (current && current->sourcePath == source.path) {
                const auto prior = std::ranges::find(values, current->variant);
                if (prior != values.end()) { index = static_cast<int>(prior - values.begin()); }
            }
            bool accepted = false;
            const auto choice = QInputDialog::getItem(this, tr("Select model variant"), tr("Variant"),
                                                        choices, index, false, &accepted);
            if (!accepted) { return; }
            variant = values.at(choices.indexOf(choice));
            if (variant) { source.document.selectVariant(variant->toStdString()); }
        }
        QStringList labels;
        std::vector<int> ids;
        int selected = 0;
        for (const auto& object : source.document.objects()) {
            if (io::canProjectBody(object)) {
                if (object.visible) {
                    selected = static_cast<int>(ids.size());
                }
                const auto* external = std::get_if<document::LinkedComponent>(&object.parameters);
                const bool assembly = std::holds_alternative<document::Assembly>(object.parameters)
                    || (external && !external->record.children.empty());
                labels.append(assembly ? tr("Assembly #%1").arg(object.id) : tr("Body #%1").arg(object.id));
                ids.push_back(object.id);
            }
        }
        if (ids.empty()) {
            throw std::runtime_error("The source model contains no available solid bodies.");
        }
        if (current) {
            const auto same = std::ranges::find(ids, current->bodyId);
            if (same != ids.end()) {
                selected = static_cast<int>(same - ids.begin());
            }
        }
        if (ids.size() > 1) {
            bool accepted = false;
            const auto body = QInputDialog::getItem(this, tr("Select source body"), tr("Body"),
                                                    labels, selected, false, &accepted);
            if (!accepted) {
                return;
            }
            selected = labels.indexOf(body);
        }
        const auto cut = current ? drawing::sectionSettings(*current) : std::nullopt;
        auto linked = io::buildModelViews(source, ids.at(selected), cut, drawing::draftSize(draft_), current,
            variant ? variant : std::optional<QString>(QString::fromStdString(source.document.configuration().activeVariant)));
        linked.variant = variant;
        setModelViews(std::move(linked),
                      tr("Link model views"));
    } catch (const std::exception& error) {
        const auto details = localizedDiagnostic(error.what());
        updateModelStatus();
        QMessageBox::warning(this, tr("Select the source model"), details);
    }
}

void DraftWindow::refreshModel()
{
    if (!draft_.modelViews) {
        return;
    }
    const auto& current = *draft_.modelViews;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        const auto source = io::openModelSource(current.sourcePath);
        if (source.digest != current.sourceDigest) {
            setModelViews(io::buildModelViews(source, current.bodyId, drawing::sectionSettings(current),
                                             drawing::draftSize(draft_), &current), tr("Update from model"));
        } else {
            updateModelStatus();
        }
    } catch (const std::exception& error) {
        updateModelStatus(localizedDiagnostic(error.what()));
    }
    QApplication::restoreOverrideCursor();
}

void DraftWindow::updateModelStatus(const QString& failure)
{
    const bool linked = draft_.modelViews.has_value();
    localRegion_->setEnabled(linked && draft_.modelViews->local.has_value());
    for (auto* action : {modelRefresh_, modelSettings_, modelRemove_, modelDetail_, sectionLine_}) {
        action->setEnabled(linked);
    }
    const auto* assembly = linked ? draft_.modelViews->assembly.get() : nullptr;
    specification_->setEnabled(assembly != nullptr);
    placePosition_->setEnabled(assembly && assembly->callouts.size() < 1000);
    removePosition_->setEnabled(assembly && !assembly->callouts.empty());
    specificationExport_->setEnabled(assembly != nullptr);
    {
        const QSignalBlocker block(specificationExport_);
        specificationExport_->setChecked(assembly && assembly->includeSpecification);
    }
    modelStatus_->setVisible(linked);
    const auto path = linked ? draft_.modelViews->sourcePath : QString{};
    const auto watched = sourceWatch_->files();
    for (const auto& previous : watched) {
        if (previous != path) {
            sourceWatch_->removePath(previous);
        }
    }
    if (!linked) {
        return;
    }
    const QFileInfo source(path);
    if (source.isFile() && !watched.contains(path)) {
        sourceWatch_->addPath(path);
    }
    const auto& model = *draft_.modelViews;
    QString problem = failure;
    if (!source.isFile()) {
        problem = tr("The source model cannot be read. Select it again to restore the link.");
    }
    if (problem.isEmpty()) {
        try {
            if (io::currentModelDigest(path) != model.sourceDigest) {
                problem = tr("The model has changed. Update from model to rebuild the views.");
            }
        } catch (const std::exception&) {
            problem = tr("The source model cannot be read. Select it again to restore the link.");
        }
    }
    auto label = tr("Model: %1 · body #%2").arg(source.fileName()).arg(model.bodyId);
    if (model.variant) {
        label += QStringLiteral(" · ") + (model.variant->isEmpty() ? tr("Base values") : tr("Variant: %1").arg(*model.variant));
    }
    modelStatus_->setToolTip(QDir::toNativeSeparators(path));
    modelStatus_->setText(problem.isEmpty() ? label : tr("%1 — stale views. %2").arg(label, problem));
}

void DraftWindow::setModelViews(std::optional<drawing::ModelViews> model, const QString& description)
{
    const auto apply = [this](const auto& value) {
        draft_.modelViews = value;
        updateModelStatus();
    };
    pushSheetEdit(description, [apply, model = std::move(model)] { apply(model); },
        [apply, model = draft_.modelViews] { apply(model); });
}

void DraftWindow::editModelViews()
{
    if (!draft_.modelViews) {
        return;
    }
    auto next = *draft_.modelViews;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("linkedViewsDialog"));
    dialog.setWindowTitle(tr("View layout and section"));
    auto* content = new QVBoxLayout(&dialog);
    auto* grid = new QGridLayout;
    content->addLayout(grid);
    const std::array columns{tr("View"), tr("Center X (mm)"), tr("Center Y (mm)"), tr("Scale factor")};
    for (int i = 0; i < 4; ++i) {
        grid->addWidget(new QLabel(columns[i], &dialog), 0, i);
    }
    const std::array names{tr("Front"), tr("Right"), tr("Top")};
    std::array<QDoubleSpinBox*, 3> xs{}, ys{}, scales{};
    for (int i = 0; i < 3; ++i) {
        grid->addWidget(new QLabel(names[i], &dialog), i + 1, 0);
        const auto spin = [&](const QString& name, int column, double low, double high, double value, int decimals) {
            auto* field = new QDoubleSpinBox(&dialog);
            field->setObjectName(name.arg(i));
            field->setAccessibleName(names[i] + QLatin1Char(' ') + columns[column]);
            field->setDecimals(decimals);
            field->setRange(low, high);
            field->setValue(value);
            grid->addWidget(field, i + 1, column);
            return field;
        };
        xs[i] = spin(QStringLiteral("linkedViewX%1"), 1, -10000, 10000, next.layout[i].center.x(), 3);
        ys[i] = spin(QStringLiteral("linkedViewY%1"), 2, -10000, 10000, next.layout[i].center.y(), 3);
        scales[i] = spin(QStringLiteral("linkedViewScale%1"), 3, 0.00000001, 1000, next.layout[i].scale, 8);
        scales[i]->setToolTip(tr("1 = 1:1, 0.5 = 1:2, 2 = 2:1"));
    }
    auto* form = new QFormLayout;
    content->addLayout(form);
    auto* plane = new QComboBox(&dialog);
    plane->setObjectName(QStringLiteral("linkedSectionPlane"));
    using geometry::Plane;
    plane->addItem(tr("None"));
    plane->addItem(tr("XZ — front"), static_cast<int>(Plane::XZ));
    plane->addItem(tr("XY — top"), static_cast<int>(Plane::XY));
    plane->addItem(tr("YZ — right"), static_cast<int>(Plane::YZ));
    form->addRow(tr("Section plane"), plane);
    auto* offset = new QDoubleSpinBox(&dialog);
    offset->setObjectName(QStringLiteral("linkedSectionOffset"));
    offset->setDecimals(3);
    offset->setRange(-100000000, 100000000);
    offset->setSuffix(tr(" mm"));
    const auto oldCut = drawing::sectionSettings(next);
    if (oldCut) {
        plane->setCurrentIndex(plane->findData(static_cast<int>(oldCut->plane)));
        offset->setValue(oldCut->offset);
    }
    form->addRow(tr("Offset from center"), offset);
    offset->setEnabled(plane->currentIndex() > 0);
    connect(plane, &QComboBox::currentIndexChanged, offset, [offset](int index) { offset->setEnabled(index > 0); });
    auto* local = new QCheckBox(tr("Local section"), &dialog);
    local->setChecked(next.local.has_value());
    form->addRow(local);
    const auto region = next.local.value_or(drawing::LocalSection{});
    const auto regionField = [&](const QString& label, double min, double value) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setRange(min, 100000);
        spin->setDecimals(3);
        spin->setValue(value);
        form->addRow(label, spin);
        return spin;
    };
    auto* localX = regionField(tr("Local section center X on view (mm)"), -100000, region.center.x());
    auto* localY = regionField(tr("Local section center Y on view (mm)"), -100000, region.center.y());
    auto* localRadius = regionField(tr("Local section radius (mm)"), .001, region.radius);
    const auto enableRegion = [=] {
        local->setEnabled(plane->currentIndex() != 0);
        for (auto* field : {localX, localY, localRadius}) {
            field->setEnabled(local->isEnabled() && local->isChecked());
        }
    };
    connect(local, &QCheckBox::toggled, &dialog, enableRegion);
    connect(plane, &QComboBox::currentIndexChanged, &dialog, enableRegion);
    enableRegion();
    auto* dimensions = new QCheckBox(tr("Dimensions and center marks"), &dialog);
    dimensions->setChecked(next.dimensions);
    form->addRow(dimensions);
    auto* hidden = new QCheckBox(tr("Hidden edges"), &dialog);
    hidden->setChecked(next.hiddenLines);
    form->addRow(hidden);
    auto* errorLabel = new QLabel(&dialog);
    errorLabel->setTextFormat(Qt::PlainText);
    errorLabel->setWordWrap(true);
    errorLabel->hide();
    content->addWidget(errorLabel);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    content->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        auto updated = next;
        for (int i = 0; i < 3; ++i) {
            updated.layout[i] = {{xs[i]->value(), ys[i]->value()}, scales[i]->value()};
        }
        updated.dimensions = dimensions->isChecked();
        updated.hiddenLines = hidden->isChecked();
        std::optional<drawing::SectionSettings> cut;
        if (plane->currentIndex() > 0) {
            cut = drawing::SectionSettings{static_cast<Plane>(plane->currentData().toInt()), offset->value()};
        }
        updated.local = cut && local->isChecked()
            ? std::optional(drawing::LocalSection{{localX->value(), localY->value()}, localRadius->value()})
            : std::nullopt;
        try {
            if (cut != oldCut) {
                if (cut) {
                    const auto source = io::openModelSource(updated.sourcePath);
                    updated = io::buildModelViews(source, updated.bodyId, cut, drawing::draftSize(draft_), &updated);
                } else {
                    auto snapshot = std::make_shared<drawing::ModelProjection>(*updated.geometry);
                    snapshot->section.reset();
                    updated.geometry = std::move(snapshot);
                }
            }
            next = std::move(updated);
            dialog.accept();
        } catch (const std::exception& error) {
            errorLabel->setText(localizedDiagnostic(error.what()));
            errorLabel->show();
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        const auto& old = *draft_.modelViews;
        if (next.layout != old.layout || next.geometry != old.geometry || next.dimensions != old.dimensions
            || next.hiddenLines != old.hiddenLines || next.local != old.local) {
            setModelViews(std::move(next), tr("Edit model views"));
        }
    }
}

void DraftWindow::placeSectionLine()
{
    const auto model = *draft_.modelViews;
    using geometry::Plane;
    auto current = drawing::sectionSettings(model);
    if (!current) {
        const QStringList names{tr("XZ — front"), tr("XY — top"), tr("YZ — right")};
        bool accepted = false;
        const auto chosen = QInputDialog::getItem(this, tr("Place section line on view"), tr("Section plane"),
                                                 names, 0, false, &accepted);
        if (!accepted) {
            return;
        }
        const std::array planes{Plane::XZ, Plane::XY, Plane::YZ};
        current = drawing::SectionSettings{planes[names.indexOf(chosen)], 0};
    }
    const bool vertical = current->plane == Plane::YZ;
    const bool top = current->plane == Plane::XZ;
    const auto placement = model.layout[top ? 2 : 0];
    const auto& view = top ? model.geometry->views.top : model.geometry->views.front;
    statusBar()->showMessage(top ? tr("Click the cutting line position on the top view.")
                                : tr("Click the cutting line position on the front view."));
    canvas_->pickPoint([this, model, placement, current, vertical, width = view.width, height = view.height](QPointF point) {
        const auto delta = (point - placement.center) / placement.scale;
        if (std::abs(delta.x()) > width / 2 || std::abs(delta.y()) > height / 2) {
            statusBar()->showMessage(tr("Click inside the indicated reference view."), 5000);
            return;
        }
        try {
            const auto source = io::openModelSource(model.sourcePath);
            const drawing::SectionSettings cut{current->plane, vertical ? delta.x() : -delta.y()};
            setModelViews(io::buildModelViews(source, model.bodyId, cut, drawing::draftSize(draft_), &model),
                          tr("Place section line on view"));
            statusBar()->clearMessage();
        } catch (const std::exception& error) {
            QMessageBox::warning(this, tr("Place section line on view"), localizedDiagnostic(error.what()));
        }
    });
}

void DraftWindow::placeLocalSection()
{
    auto model = *draft_.modelViews;
    using geometry::Plane;
    const auto plane = model.geometry->section->plane;
    const int index = plane == Plane::XZ ? 0 : plane == Plane::YZ ? 1 : 2;
    const auto placement = model.layout[index];
    const auto& view = model.geometry->section->view;
    statusBar()->showMessage(tr("Click the center of the local section on the sectioned view."));
    canvas_->pickPoint([this, model, placement, width = view.width, height = view.height](QPointF point) mutable {
        const auto delta = (point - placement.center) / placement.scale;
        if (std::abs(delta.x()) > width / 2 || std::abs(delta.y()) > height / 2) {
            statusBar()->showMessage(tr("Click inside the indicated reference view."), 5000);
            return;
        }
        model.local->center = {delta.x(), -delta.y()};
        setModelViews(std::move(model), tr("Place local section area"));
        statusBar()->clearMessage();
    });
}

void DraftWindow::editDetail()
{
    const auto model = *draft_.modelViews;
    const auto detail = model.detail.value_or(drawing::DetailView{});
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Detail view"));
    auto* form = new QFormLayout(&dialog);
    auto* enabled = new QCheckBox(tr("Show detail view"), &dialog);
    enabled->setChecked(true);
    form->addRow(enabled);
    auto* source = new QComboBox(&dialog);
    source->addItems({tr("Front"), tr("Right"), tr("Top")});
    source->setCurrentIndex(detail.source);
    form->addRow(tr("Reference view"), source);
    const auto field = [&](const QString& label, double min, double max, double value) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setRange(min, max);
        spin->setDecimals(3);
        spin->setValue(value);
        form->addRow(label, spin);
        return spin;
    };
    auto* radius = field(tr("Detail radius on model (mm)"), .001, 100000, detail.radius);
    auto* scale = field(tr("Scale factor"), .001, 1000, detail.placement.scale);
    auto* x = field(tr("Center X (mm)"), -10000, 10000, detail.placement.center.x());
    auto* y = field(tr("Center Y (mm)"), -10000, 10000, detail.placement.center.y());
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (!enabled->isChecked()) {
        if (model.detail) {
            auto updated = model;
            updated.detail.reset();
            setModelViews(std::move(updated), tr("Remove detail view"));
        }
        return;
    }
    auto updated = model;
    const int index = source->currentIndex();
    updated.detail = drawing::DetailView{index, detail.center, radius->value(), {{x->value(), y->value()}, scale->value()}};
    const auto place = model.layout[index];
    const auto& views = model.geometry->views;
    const auto& view = *std::array{&views.front, &views.right, &views.top}[index];
    statusBar()->showMessage(tr("Click the center of the detail on the selected reference view."));
    canvas_->pickPoint([this, updated, place, width = view.width, height = view.height](QPointF point) mutable {
        const auto delta = (point - place.center) / place.scale;
        if (std::abs(delta.x()) > width / 2 || std::abs(delta.y()) > height / 2) {
            statusBar()->showMessage(tr("Click inside the indicated reference view."), 5000);
            return;
        }
        updated.detail->center = {delta.x(), -delta.y()};
        setModelViews(std::move(updated), tr("Edit detail view"));
        statusBar()->clearMessage();
    });
}

void DraftWindow::prepareModelDimensions()
{
    auto& model = draft_.modelViews;
    if (!model || !model->geometry || !model->geometry->views.top.id.isNull()) {
        return;
    }
    try {
        const auto source = io::openModelSource(model->sourcePath);
        if (source.digest == model->sourceDigest) {
            model = io::buildModelViews(source, model->bodyId, drawing::sectionSettings(*model),
                                         drawing::draftSize(draft_), &*model);
        }
    } catch (const std::exception&) {
        // Keep the saved views available for drawing and use the normal refresh command to diagnose the source.
    }
}

void DraftWindow::reattachDimension()
{
    prepareModelDimensions();
    const int index = canvas_->selected();
    if (index < 0 || !draft_.modelViews) {
        return;
    }
    auto item = draft_.items[index];
    const auto kind = item.kind;
    const bool round = kind == DraftKind::Radius || kind == DraftKind::Diameter;
    if (!round && !drawing::isLinearDimension(kind)) {
        statusBar()->showMessage(tr("Select a linear, radial or diameter dimension."), 4000);
        return;
    }
    bool first = true;
    if (!round) {
        const QStringList points{tr("First measured point"), tr("Second measured point")};
        bool accepted = false;
        const auto choice = QInputDialog::getItem(this, tr("Reattach dimension to view"), tr("Measured point"),
                                                 points, 0, false, &accepted);
        if (!accepted) {
            return;
        }
        first = choice == points.front();
    }
    statusBar()->showMessage(round ? tr("Select a circular edge on a model view.")
                                   : tr("Select an edge or vertex on the same model view."));
    canvas_->pickPoint([this, index, item = std::move(item), first, round](QPointF position) mutable {
        const auto picked = canvas_->modelPick(position, round);
        if (!picked) {
            statusBar()->showMessage(tr("No model geometry at the selected point."), 4000);
            return;
        }
        if (round) {
            auto center = picked->reference;
            center.anchor = drawing::DraftAnchor::ModelCenter;
            center.parameter = 0;
            if (const auto origin = drawing::DraftGeometry(draft_).point(center)) {
                item.anchorA = center;
                item.anchorB = picked->reference;
                item.a = *origin;
                item.b = picked->position;
            }
        } else {
            const auto& other = first ? item.anchorB : item.anchorA;
            if (other && drawing::isModelReference(*other) && other->view != picked->reference.view) {
                statusBar()->showMessage(tr("Both measured points must belong to the same view."), 4000);
                return;
            }
            (first ? item.anchorA : item.anchorB) = picked->reference;
            (first ? item.a : item.b) = picked->position;
        }
        editItem(index, item, tr("Reattach dimension"));
        statusBar()->clearMessage();
    });
}

void DraftWindow::editSelected()
{
    const int index = canvas_->selected();
    if (index < 0) {
        return;
    }
    auto item = draft_.items[index];
    if (item.kind == DraftKind::Table) {
        editTable(index);
        return;
    }
    if (item.kind == DraftKind::Polyline || item.kind == DraftKind::Hatch || item.kind == DraftKind::Spline) {
        return;
    }
    const auto measured = drawing::DraftGeometry(draft_).resolve(item);
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("draftItemDialog"));
    dialog.setWindowTitle(tr("Item properties"));
    auto* form = new QFormLayout(&dialog);
    struct CoordinateFields {
        QDoubleSpinBox* x;
        QDoubleSpinBox* y;
        QPointF original;
        QPointF displayed;
        QPointF point() const
        {
            return {x->value() == displayed.x() ? original.x() : x->value(),
                    y->value() == displayed.y() ? original.y() : y->value()};
        }
    };
    const auto pair = [&](const QString& xName, const QString& yName, QPointF point, bool linked = false) {
        auto* x = new QDoubleSpinBox(&dialog);
        auto* y = new QDoubleSpinBox(&dialog);
        for (auto* field : {x, y}) {
            field->setDecimals(3);
            field->setRange(-10000, 10000);
            field->setSuffix(tr(" mm"));
        }
        if (linked) {
            for (auto* field : {x, y}) {
                field->setReadOnly(true);
                field->setToolTip(tr("This point follows the attached geometry."));
            }
        }
        x->setValue(point.x());
        y->setValue(point.y());
        form->addRow(xName, x);
        form->addRow(yName, y);
        return CoordinateFields{x, y, point, {x->value(), y->value()}};
    };
    const auto a = pair(tr("First point X"), tr("First point Y"), measured.a, item.anchorA.has_value());
    std::optional<CoordinateFields> b, label, third;
    QLineEdit* text = nullptr;
    QCheckBox* weldReverse = nullptr;
    if (item.kind == DraftKind::Weld) {
        weldReverse = new QCheckBox(tr("Weld notation below the shelf"), &dialog);
        weldReverse->setObjectName(QStringLiteral("weldReverse"));
        weldReverse->setChecked(item.weldReverse);
        form->addRow(weldReverse);
    }
    if (item.kind == DraftKind::Text || drawing::isAnnotation(item.kind)) {
        text = new QLineEdit(item.text, &dialog);
        text->setMaxLength(200);
        form->addRow(tr("Text"), text);
    } else if (item.kind != DraftKind::Fragment) {
        b = pair(tr("Second point X"), tr("Second point Y"), measured.b, item.anchorB.has_value());
    }
    if (drawing::isAnnotation(item.kind)) {
        label = pair(tr("Label X"), tr("Label Y"), measured.label);
    } else if (drawing::isDimension(item.kind)) {
        label = pair(tr("Dimension line X"), tr("Dimension line Y"), measured.label);
    } else if (item.kind == DraftKind::Arc) {
        label = pair(tr("End point X"), tr("End point Y"), measured.label);
    }
    auto* fitNote = new QLineEdit(item.dimensionNote.fit, &dialog);
    auto* upper = new QLineEdit(item.dimensionNote.upper, &dialog);
    auto* lower = new QLineEdit(item.dimensionNote.lower, &dialog);
    if (drawing::isDimension(item.kind)) {
        fitNote->setObjectName(QStringLiteral("dimensionFit"));
        upper->setObjectName(QStringLiteral("dimensionUpper"));
        lower->setObjectName(QStringLiteral("dimensionLower"));
        fitNote->setMaxLength(24);
        upper->setMaxLength(16);
        lower->setMaxLength(16);
        form->addRow(tr("Fit code"), fitNote);
        form->addRow(tr("Upper deviation"), upper);
        form->addRow(tr("Lower deviation"), lower);
    } else {
        fitNote->hide();
        upper->hide();
        lower->hide();
    }
    if (item.kind == DraftKind::Angular) {
        third = pair(tr("Third point X"), tr("Third point Y"), measured.points.front(), item.anchorC.has_value());
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!item.anchorA) {
            item.a = a.point();
        }
        if (b && !item.anchorB) {
            item.b = b->point();
        }
        if (label) {
            item.label += label->point() - measured.label;
        }
        if (third && !item.anchorC) {
            item.points.front() = third->point();
        }
        if (text) {
            item.text = text->text();
        }
        if (weldReverse) {
            item.weldReverse = weldReverse->isChecked();
        }
        if (drawing::isDimension(item.kind)) {
            item.dimensionNote = {fitNote->text().trimmed(), upper->text().trimmed(), lower->text().trimmed()};
        }
        if (drawing::validItem(item)) {
            dialog.accept();
        } else {
            QMessageBox::warning(&dialog, tr("Item properties"), tr("Enter a valid size or non-empty text."));
        }
    });
    if (dialog.exec() == QDialog::Accepted && item != draft_.items[index]) {
        replaceCurve(index, {item}, tr("Edit item"));
    }
}

void DraftWindow::editSheet()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("draftSheetDialog"));
    dialog.setWindowTitle(tr("Format and title block"));
    auto* form = new QFormLayout(&dialog);
    auto* paper = new QComboBox(&dialog);
    paper->setObjectName(QStringLiteral("draftPaper"));
    paper->addItems({QStringLiteral("A4"), QStringLiteral("A3")});
    paper->setCurrentIndex(draft_.a3 ? 1 : 0);
    form->addRow(tr("Sheet format"), paper);
    const auto field = [&](const QString& label, const char* name, const QString& value) {
        auto* input = new QLineEdit(value, &dialog);
        input->setObjectName(QString::fromLatin1(name));
        input->setMaxLength(200);
        form->addRow(label, input);
        return input;
    };
    auto* orientation = new QComboBox(&dialog);
    orientation->setObjectName(QStringLiteral("draftOrientation"));
    orientation->addItems({tr("Landscape"), tr("Portrait")});
    orientation->setCurrentIndex(draft_.paper.portrait ? 1 : 0);
    form->addRow(tr("Orientation"), orientation);
    auto* eskd = new QCheckBox(tr("ESKD title block"), &dialog);
    eskd->setChecked(draft_.paper.eskd);
    form->addRow(eskd);
    const auto requirePortrait = [=] {
        const bool required = eskd->isChecked() && paper->currentIndex() == 0;
        if (required) {
            orientation->setCurrentIndex(1);
        }
        orientation->setEnabled(!required);
    };
    connect(eskd, &QCheckBox::toggled, &dialog, requirePortrait);
    connect(paper, &QComboBox::currentIndexChanged, &dialog, requirePortrait);
    requirePortrait();
    auto* material = field(tr("Material"), "draftMaterial", draft_.paper.material);
    auto* organization = field(tr("Organization"), "draftOrganization", draft_.paper.organization);
    auto* title = field(tr("Name"), "draftTitle", draft_.title);
    auto* number = field(tr("Drawing number"), "draftNumber", draft_.number);
    auto* author = field(tr("Drawn by"), "draftAuthor", draft_.author);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto apply = [this](bool a3, const drawing::DraftPaper& paper, const QString& name,
                              const QString& number, const QString& author) {
        const bool resize = a3 != draft_.a3 || paper.portrait != draft_.paper.portrait;
        draft_.paper = paper;
        draft_.a3 = a3;
        draft_.title = name;
        draft_.number = number;
        draft_.author = author;
        if (resize) {
            canvas_->fit();
        }
    };
    const bool a3 = paper->currentIndex() == 1;
    auto newPaper = draft_.paper;
    newPaper.portrait = orientation->currentIndex() == 1;
    newPaper.eskd = eskd->isChecked();
    newPaper.material = material->text();
    newPaper.organization = organization->text();
    if (a3 != draft_.a3 || newPaper != draft_.paper || title->text() != draft_.title
        || number->text() != draft_.number || author->text() != draft_.author) {
        pushSheetEdit(tr("Edit sheet"),
            [apply, a3, newPaper, title = title->text(), number = number->text(), author = author->text()] {
                apply(a3, newPaper, title, number, author);
            },
            [apply, a3 = draft_.a3, paper = draft_.paper, title = draft_.title, number = draft_.number, author = draft_.author] {
                apply(a3, paper, title, number, author);
            });
    }
}

bool DraftWindow::confirmSave()
{
    if (history_.isClean()) {
        return true;
    }
    const auto result = QMessageBox::warning(this, tr("Unsaved drawing"), tr("Save changes to the drawing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return result == QMessageBox::Discard || (result == QMessageBox::Save && saveDrawing(false));
}

void DraftWindow::closeEvent(QCloseEvent* event)
{
    if (confirmSave()) {
        event->accept();
    } else {
        event->ignore();
    }
}

}  // namespace graph3d
