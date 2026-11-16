#include "gui/DraftWindow.h"

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
#include <QToolBar>
#include <QUndoCommand>
#include <QVBoxLayout>

#include <algorithm>
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

DraftWindow::DraftWindow(QWidget* parent)
    : QMainWindow(parent, Qt::Window), sourceWatch_(new QFileSystemWatcher(this)),
      modelStatus_(new QLabel(this))
{
    setObjectName(QStringLiteral("draftWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1100, 760);
    auto* workspace = new QWidget(this);
    auto* layout = new QVBoxLayout(workspace);
    layout->setContentsMargins(8, 0, 8, 0);
    auto* lineInput = new LineInput(workspace);
    canvas_ = new DraftCanvas(draft_, *lineInput, workspace);
    layout->addWidget(lineInput);
    layout->addWidget(canvas_, 1);
    setCentralWidget(workspace);
    connect(canvas_, &DraftCanvas::lineOutsideSheet, this, [this] {
        statusBar()->showMessage(tr("The line endpoint must be inside the drawing area."), 4000);
    });
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("New drawing"), QKeySequence::New, this, [this] { newDrawing(); });
    file->addAction(tr("Open drawing…"), QKeySequence::Open, this, &DraftWindow::browseDrawing);
    auto* save = file->addAction(tr("Save"), QKeySequence::Save, this, [this] { saveDrawing(false); });
    save->setObjectName(QStringLiteral("saveDraft"));
    file->addAction(tr("Save as…"), QKeySequence::SaveAs, this, [this] { saveDrawing(true); });
    auto* pdf = file->addAction(tr("Export PDF…"), this, &DraftWindow::exportPdf);
    pdf->setObjectName(QStringLiteral("exportDraftPdf"));
    file->addSeparator();
    file->addAction(tr("Close drawing"), QKeySequence::Close, this, &QWidget::close);

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
    remove->setObjectName(QStringLiteral("removeDraftItem"));
    remove->setEnabled(false);
    properties->setEnabled(false);
    connect(canvas_, &DraftCanvas::selectionChanged, this, [remove, properties](int selected) {
        remove->setEnabled(selected >= 0);
        properties->setEnabled(selected >= 0);
    });
    edit->addAction(tr("Cancel tool"), Qt::Key_Escape, canvas_, &DraftCanvas::cancel);
    auto* sheet = menuBar()->addMenu(tr("&Sheet"));
    auto* settings = sheet->addAction(tr("Format and title block…"), this, &DraftWindow::editSheet);
    settings->setObjectName(QStringLiteral("draftSheetSettings"));
    auto* fit = sheet->addAction(tr("Fit sheet"), Qt::Key_F, canvas_, &DraftCanvas::fit);
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
    modelRemove_ = views->addAction(tr("Remove model views"), this, [this] {
        setModelViews({}, tr("Remove model views"));
    });
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
    const auto tool = [&](const QString& name, const char* objectName, std::optional<DraftKind> kind, const QString& hint) {
        auto* action = bar->addAction(name);
        action->setObjectName(QString::fromLatin1(objectName));
        action->setCheckable(true);
        action->setToolTip(hint);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, kind, hint] {
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
    auto* stroke = new QComboBox(bar);
    stroke->setAccessibleName(tr("Line style"));
    stroke->addItems({tr("Solid"), tr("Hidden"), tr("Center")});
    bar->addWidget(stroke);
    connect(stroke, &QComboBox::currentIndexChanged, this, [this](int value) {
        const auto line = static_cast<DraftStroke>(value);
        canvas_->setStroke(line);
        if (const int index = canvas_->selected(); index >= 0) {
            auto item = draft_.items[index];
            if (item.kind == DraftKind::Line || item.kind == DraftKind::Rectangle || item.kind == DraftKind::Circle) {
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
        identified.id = QUuid::createUuid();
        editItem(static_cast<int>(draft_.items.size()), identified, tr("Create item"));
    });
    connect(canvas_, &DraftCanvas::itemMoved, this, [this](int index, const DraftItem& item) {
        editItem(index, item, tr("Move item"));
    });
    connect(&history_, &QUndoStack::indexChanged, this, [this] {
        canvas_->cancel();
        canvas_->update();
    });
    connect(&history_, &QUndoStack::cleanChanged, this, [this] { updateTitle(); });
    select->trigger();
    updateTitle();
}

void DraftWindow::removeItem(int index)
{
    const auto id = draft_.items[index].id;
    const drawing::DraftGeometry geometry(draft_);
    history_.beginMacro(tr("Delete item"));
    for (int i = 0; i < static_cast<int>(draft_.items.size()); ++i) {
        const auto& dimension = draft_.items[i];
        const bool detachA = dimension.anchorA && dimension.anchorA->item == id;
        const bool detachB = dimension.anchorB && dimension.anchorB->item == id;
        if (detachA || detachB) {
            auto fixed = geometry.resolve(dimension);
            if (detachA) {
                fixed.anchorA.reset();
            }
            if (detachB) {
                fixed.anchorB.reset();
            }
            editItem(i, fixed, tr("Detach dimension point"));
        }
    }
    editItem(index, {}, tr("Delete item"));
    history_.endMacro();
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
    history_.push(new DraftEdit(description, [=] { apply(before.has_value(), after); },
                               [=] { apply(after.has_value(), before); }));
}

void DraftWindow::replaceDrawing(drawing::Draft draft, const QString& path)
{
    canvas_->cancel();
    canvas_->select(-1);
    draft_ = std::move(draft);
    path_ = path;
    history_.clear();
    canvas_->fit();
    updateModelStatus();
    updateTitle();
}

void DraftWindow::updateTitle()
{
    setWindowTitle(tr("%1[*] — Graph3D Drawing").arg(path_.isEmpty() ? tr("Untitled") : QFileInfo(path_).fileName()));
    setWindowModified(!history_.isClean());
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
    try {
        io::saveDraft(path, draft_);
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

void DraftWindow::exportPdf()
{
    const auto path = chooseSavePath(this, tr("Export drawing PDF"), {}, tr("PDF files (*.pdf)"), QStringLiteral("pdf"));
    if (path.isEmpty()) {
        return;
    }
    try {
        drawing::exportDraftPdf(path, draft_);
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
        const auto source = io::openModelSource(path);
        QStringList labels;
        std::vector<int> ids;
        int selected = 0;
        for (const auto& object : source.document.objects()) {
            if (io::canProjectBody(object)) {
                if (object.visible) {
                    selected = static_cast<int>(ids.size());
                }
                labels.append(tr("Body #%1").arg(object.id));
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
        setModelViews(io::buildModelViews(source, ids.at(selected), cut, drawing::draftSize(draft_), current),
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
    for (auto* action : {modelRefresh_, modelSettings_, modelRemove_}) {
        action->setEnabled(linked);
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
    const auto label = tr("Model: %1 · body #%2").arg(source.fileName()).arg(model.bodyId);
    modelStatus_->setToolTip(QDir::toNativeSeparators(path));
    modelStatus_->setText(problem.isEmpty() ? label : tr("%1 — stale views. %2").arg(label, problem));
}

void DraftWindow::setModelViews(std::optional<drawing::ModelViews> model, const QString& description)
{
    const auto apply = [this](const auto& value) {
        draft_.modelViews = value;
        updateModelStatus();
    };
    history_.push(new DraftEdit(description, [apply, model = std::move(model)] { apply(model); },
        [apply, model = draft_.modelViews] { apply(model); }));
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
            || next.hiddenLines != old.hiddenLines) {
            setModelViews(std::move(next), tr("Edit model views"));
        }
    }
}

void DraftWindow::editSelected()
{
    const int index = canvas_->selected();
    if (index < 0) {
        return;
    }
    auto item = draft_.items[index];
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
    std::optional<CoordinateFields> b, label;
    QLineEdit* text = nullptr;
    if (item.kind == DraftKind::Text) {
        text = new QLineEdit(item.text, &dialog);
        text->setMaxLength(200);
        form->addRow(tr("Text"), text);
    } else {
        b = pair(tr("Second point X"), tr("Second point Y"), measured.b, item.anchorB.has_value());
    }
    if (item.kind == DraftKind::Horizontal || item.kind == DraftKind::Vertical) {
        label = pair(tr("Dimension line X"), tr("Dimension line Y"), measured.label);
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
        if (text) {
            item.text = text->text();
        }
        if (drawing::validItem(item)) {
            dialog.accept();
        } else {
            QMessageBox::warning(&dialog, tr("Item properties"), tr("Enter a valid size or non-empty text."));
        }
    });
    if (dialog.exec() == QDialog::Accepted && item != draft_.items[index]) {
        editItem(index, item, tr("Edit item"));
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
    const auto apply = [this](bool a3, const QString& name, const QString& number, const QString& author) {
        const bool resize = a3 != draft_.a3;
        draft_.a3 = a3;
        draft_.title = name;
        draft_.number = number;
        draft_.author = author;
        if (resize) {
            canvas_->fit();
        }
    };
    const bool a3 = paper->currentIndex() == 1;
    if (a3 != draft_.a3 || title->text() != draft_.title || number->text() != draft_.number
        || author->text() != draft_.author) {
        history_.push(new DraftEdit(tr("Edit sheet"),
            [apply, a3, title = title->text(), number = number->text(), author = author->text()] { apply(a3, title, number, author); },
            [apply, a3 = draft_.a3, title = draft_.title, number = draft_.number, author = draft_.author] { apply(a3, title, number, author); }));
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
