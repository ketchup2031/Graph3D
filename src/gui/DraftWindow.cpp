#include "gui/DraftWindow.h"

#include "gui/DraftCanvas.h"
#include "gui/InterfaceLanguage.h"
#include "gui/UserPreferences.h"
#include "io/DraftIO.h"

#include <QActionGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>
#include <QUndoCommand>

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
    : QMainWindow(parent, Qt::Window), canvas_(new DraftCanvas(draft_, this))
{
    setObjectName(QStringLiteral("draftWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1100, 760);
    setCentralWidget(canvas_);
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
            editItem(index, {}, tr("Delete item"));
        }
    });
    auto* properties = edit->addAction(tr("Item properties…"), this, &DraftWindow::editSelected);
    properties->setObjectName(QStringLiteral("draftItemProperties"));
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
    tool(tr("Line"), "draftLine", DraftKind::Line, tr("Pick the start and end of the line."));
    tool(tr("Rectangle"), "draftRect", DraftKind::Rectangle, tr("Pick two opposite corners."));
    tool(tr("Circle"), "draftCircle", DraftKind::Circle, tr("Pick the center and a point on the circle."));
    tool(tr("Text"), "draftText", DraftKind::Text, tr("Pick a position for a note."));
    bar->addSeparator();
    tool(tr("Horizontal dim."), "draftHorizontal", DraftKind::Horizontal, tr("Pick two points, then place the dimension line."));
    tool(tr("Vertical dim."), "draftVertical", DraftKind::Vertical, tr("Pick two points, then place the dimension line."));
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
        editItem(static_cast<int>(draft_.items.size()), item, tr("Create item"));
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

void DraftWindow::editSelected()
{
    const int index = canvas_->selected();
    if (index < 0) {
        return;
    }
    auto item = draft_.items[index];
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("draftItemDialog"));
    dialog.setWindowTitle(tr("Item properties"));
    auto* form = new QFormLayout(&dialog);
    struct CoordinateFields {
        QDoubleSpinBox* x;
        QDoubleSpinBox* y;
        QPointF point() const { return {x->value(), y->value()}; }
    };
    const auto pair = [&](const QString& xName, const QString& yName, QPointF point) {
        auto* x = new QDoubleSpinBox(&dialog);
        auto* y = new QDoubleSpinBox(&dialog);
        for (auto* field : {x, y}) {
            field->setDecimals(3);
            field->setRange(-10000, 10000);
            field->setSuffix(tr(" mm"));
        }
        x->setValue(point.x());
        y->setValue(point.y());
        form->addRow(xName, x);
        form->addRow(yName, y);
        return CoordinateFields{x, y};
    };
    const auto a = pair(tr("First point X"), tr("First point Y"), item.a);
    std::optional<CoordinateFields> b, label;
    QLineEdit* text = nullptr;
    if (item.kind == DraftKind::Text) {
        text = new QLineEdit(item.text, &dialog);
        text->setMaxLength(200);
        form->addRow(tr("Text"), text);
    } else {
        b = pair(tr("Second point X"), tr("Second point Y"), item.b);
    }
    if (item.kind == DraftKind::Horizontal || item.kind == DraftKind::Vertical) {
        label = pair(tr("Dimension line X"), tr("Dimension line Y"), item.label);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        item.a = a.point();
        if (b) {
            item.b = b->point();
        }
        if (label) {
            item.label = label->point();
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
