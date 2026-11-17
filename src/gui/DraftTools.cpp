#include "gui/DraftWindow.h"

#include "gui/DocumentWindow.h"
#include "gui/DraftCanvas.h"
#include "gui/InterfaceLanguage.h"
#include "gui/UserPreferences.h"
#include "io/DraftIO.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>

#include <algorithm>
#include <stdexcept>

namespace graph3d {
namespace {

using namespace drawing;

QString drawingFile(QWidget* parent, const QString& title, const QString& filter, const char* suffix, bool save)
{
    QFileDialog dialog(parent, title, {}, filter);
    dialog.setOption(QFileDialog::DontUseNativeDialog, darkThemeEnabled() || russianInterface());
    dialog.setAcceptMode(save ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    dialog.setFileMode(save ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
    if (save) {
        dialog.setDefaultSuffix(QString::fromLatin1(suffix));
    }
    return dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()
        ? dialog.selectedFiles().front() : QString{};
}

QString titleFieldLabel(TitleField field)
{
    constexpr std::array names{"Fixed text", "Drawing title", "Designation", "Author", "Material",
                                "Organization", "Mass (kg)", "Sheet number", "Number of sheets"};
    return QCoreApplication::translate("TitleBlock", names.at(static_cast<std::size_t>(field)));
}

QDoubleSpinBox* sizeField(QFormLayout& form, const QString& label, const char* name,
                          double value, double min, double max)
{
    auto* field = new QDoubleSpinBox(form.parentWidget());
    field->setObjectName(QString::fromLatin1(name));
    field->setDecimals(2);
    field->setRange(min, max);
    field->setValue(value);
    form.addRow(label, field);
    return field;
}

QString cellText(const QTableWidget& grid, int row, int column)
{
    const auto* cell = grid.item(row, column);
    return cell ? cell->text() : QString{};
}

}  // namespace

void DraftWindow::editTitleBlock()
{
    TitleBlockLayout initial{130, 6, {
        {tr("Drawing title"), TitleField::Title, {}}, {tr("Designation"), TitleField::Designation, {}},
        {tr("Author"), TitleField::Author, {}}, {tr("Mass (kg)"), TitleField::Mass, {}},
        {tr("Sheet number"), TitleField::Page, {}}}};
    if (draft_.paper.titleBlock) {
        initial = *draft_.paper.titleBlock;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("draftTitleBlockEditor"));
    dialog.setWindowTitle(tr("Edit custom title block"));
    dialog.resize(560, 430);
    auto* form = new QFormLayout(&dialog);
    auto* custom = new QCheckBox(tr("Use a custom title block"), &dialog);
    custom->setObjectName(QStringLiteral("customTitleBlockEnabled"));
    custom->setChecked(true);
    form->addRow(custom);
    auto* fromModel = new QCheckBox(tr("Use linked model title, designation and mass"), &dialog);
    fromModel->setObjectName(QStringLiteral("useModelTitleFields"));
    fromModel->setChecked(draft_.paper.modelTitle);
    form->addRow(fromModel);
    auto* width = sizeField(*form, tr("Block width (mm)"), "titleBlockWidth", initial.width, 40, 185);
    auto* height = sizeField(*form, tr("Row height (mm)"), "titleBlockRowHeight", initial.rowHeight, 4, 12);
    auto* count = new QSpinBox(&dialog);
    count->setObjectName(QStringLiteral("titleBlockRows"));
    count->setRange(1, 12);
    count->setValue(static_cast<int>(initial.rows.size()));
    form->addRow(tr("Rows"), count);
    auto* grid = new QTableWidget(count->value(), 3, &dialog);
    grid->setObjectName(QStringLiteral("titleBlockFields"));
    grid->setHorizontalHeaderLabels({tr("Label"), tr("Field"), tr("Fixed text")});
    grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    form->addRow(grid);
    const auto addRow = [&](int i, const TitleBlockRow& row) {
        grid->setItem(i, 0, new QTableWidgetItem(row.label));
        grid->setItem(i, 2, new QTableWidgetItem(row.text));
        auto* picker = new QComboBox(grid);
        for (int option = 0; option <= static_cast<int>(TitleField::Pages); ++option) {
            picker->addItem(titleFieldLabel(static_cast<TitleField>(option)), option);
        }
        picker->setCurrentIndex(static_cast<int>(row.field));
        grid->setCellWidget(i, 1, picker);
    };
    for (int i = 0; i < count->value(); ++i) {
        addRow(i, initial.rows[i]);
    }
    connect(count, &QSpinBox::valueChanged, &dialog, [=, &addRow](int rows) {
        const int previous = grid->rowCount();
        grid->setRowCount(rows);
        for (int i = previous; i < rows; ++i) {
            addRow(i, {});
        }
    });
    const auto enable = [=](bool enabled) {
        width->setEnabled(enabled);
        height->setEnabled(enabled);
        count->setEnabled(enabled);
        grid->setEnabled(enabled);
    };
    connect(custom, &QCheckBox::toggled, &dialog, enable);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    auto after = draft_.paper;
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        after.modelTitle = fromModel->isChecked();
        after.titleBlock.reset();
        if (custom->isChecked()) {
            TitleBlockLayout current{width->value(), height->value(), {}};
            for (int i = 0; i < grid->rowCount(); ++i) {
                const auto* picker = qobject_cast<QComboBox*>(grid->cellWidget(i, 1));
                current.rows.push_back({cellText(*grid, i, 0), static_cast<TitleField>(picker->currentData().toInt()),
                                        cellText(*grid, i, 2)});
            }
            if (!drawing::validTitleBlock(current)) {
                QMessageBox::warning(&dialog, tr("Edit custom title block"), tr("Use valid sizes and shorter labels."));
                return;
            }
            after.titleBlock = std::move(current);
        }
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted && after != draft_.paper) {
        pushSheetEdit(tr("Edit title block"), [this, after] { draft_.paper = after; },
                      [this, before = draft_.paper] { draft_.paper = before; });
    }
}

void DraftWindow::saveDrawingTemplate()
{
    const auto path = drawingFile(this, tr("Save drawing template"), tr("Graph3D templates (*.g3template)"),
                                  "g3template", true);
    if (path.isEmpty()) {
        return;
    }
    auto result = wholeDrawing();
    const auto clear = [](Draft& sheet) {
        sheet.items.clear();
        sheet.modelViews.reset();
    };
    clear(result);
    for (auto& sheet : result.additionalSheets) {
        clear(sheet);
    }
    try {
        io::saveDraft(path, result);
        statusBar()->showMessage(tr("Drawing template saved."), 4000);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Save drawing template"), localizedDiagnostic(error.what()));
    }
}

void DraftWindow::newFromDrawingTemplate()
{
    const auto path = drawingFile(this, tr("New from drawing template"), tr("Graph3D templates (*.g3template)"),
                                  "g3template", false);
    if (path.isEmpty()) {
        return;
    }
    try {
        auto result = io::openDraft(path);
        if (host_) {
            auto* editor = new DraftWindow(host_, host_);
            editor->replaceDrawing(std::move(result));
            host_->addDocument(editor);
            editor->history_.resetClean();
        } else if (confirmSave()) {
            replaceDrawing(std::move(result));
            history_.resetClean();
        }
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("New from drawing template"), localizedDiagnostic(error.what()));
    }
}

void DraftWindow::saveFragment()
{
    Draft result;
    const DraftGeometry geometry(draft_);
    QRectF bounds;
    for (const auto& item : draft_.items) {
        if (!layerVisible(draft_, item.layer) || item.kind == DraftKind::ConstructionLine
            || item.kind == DraftKind::Fragment) {
            continue;
        }
        auto piece = geometry.resolve(item);
        if (piece.lostReference) {
            continue;
        }
        // The fragment keeps the displayed geometry and measurements at insertion time.
        if (isDimension(piece.kind) && piece.measurementScale != 1) {
            continue;
        }
        piece.anchorA.reset();
        piece.anchorB.reset();
        piece.anchorC.reset();
        piece.hatchBoundaries.clear();
        piece.layer = 0;
        bounds |= draftItemBounds(piece);
        result.items.push_back(std::move(piece));
    }
    if (result.items.empty() || result.items.size() > 1024) {
        QMessageBox::information(this, tr("Save drawing fragment"), tr("A fragment needs 1 to 1024 visible drawing objects."));
        return;
    }
    for (auto& item : result.items) {
        item = translateDraftItem(std::move(item), -bounds.topLeft());
    }
    const auto path = drawingFile(this, tr("Save drawing fragment"), tr("Graph3D fragments (*.g3frag)"), "g3frag", true);
    if (path.isEmpty()) {
        return;
    }
    try {
        io::saveDraft(path, result);
        statusBar()->showMessage(tr("Drawing fragment saved."), 4000);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Save drawing fragment"), localizedDiagnostic(error.what()));
    }
}

void DraftWindow::insertFragment()
{
    const auto path = drawingFile(this, tr("Insert fragment"), tr("Graph3D fragments (*.g3frag)"), "g3frag", false);
    if (path.isEmpty()) {
        return;
    }
    try {
        auto result = io::openDraft(path);
        DraftItem item{DraftKind::Fragment};
        item.fragment = std::move(result.items);
        if (!drawing::validItem(item) || result.modelViews || !result.additionalSheets.empty()) {
            throw std::invalid_argument("Invalid drawing fragment.");
        }
        statusBar()->showMessage(tr("Choose the fragment insertion point."));
        canvas_->pickPoint([this, item = std::move(item)](QPointF point) mutable {
            item.a = point;
            item.id = QUuid::createUuid();
            item.layer = layers_->currentIndex();
            if (layerEditable(draft_, item.layer)) {
                editItem(static_cast<int>(draft_.items.size()), std::move(item), tr("Insert fragment"));
            } else {
                statusBar()->showMessage(tr("Select a visible, unlocked layer to draw."), 4000);
            }
        });
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Insert fragment"), localizedDiagnostic(error.what()));
    }
}

void DraftWindow::editTable(int index)
{
    DraftItem item = index < 0 ? DraftItem{DraftKind::Table} : draft_.items[index];
    const auto initial = item.table.value_or(DraftTable{2, 90, 6, {tr("Name"), tr("Value"), {}, {}}});
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("draftTableEditor"));
    dialog.setWindowTitle(tr("Drawing table"));
    dialog.resize(510, 390);
    auto* form = new QFormLayout(&dialog);
    auto* rows = new QSpinBox(&dialog);
    rows->setObjectName(QStringLiteral("draftTableRows"));
    rows->setRange(1, 30);
    rows->setValue(static_cast<int>(initial.cells.size() / initial.columns));
    form->addRow(tr("Rows"), rows);
    auto* columns = new QSpinBox(&dialog);
    columns->setObjectName(QStringLiteral("draftTableColumns"));
    columns->setRange(1, 10);
    columns->setValue(initial.columns);
    form->addRow(tr("Columns"), columns);
    auto* width = sizeField(*form, tr("Table width (mm)"), "draftTableWidth", initial.width, 10, 360);
    auto* height = sizeField(*form, tr("Row height (mm)"), "draftTableHeight", initial.rowHeight, 3, 30);
    auto* grid = new QTableWidget(rows->value(), columns->value(), &dialog);
    grid->setObjectName(QStringLiteral("draftTableCells"));
    grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    form->addRow(grid);
    for (int r = 0; r < grid->rowCount(); ++r) {
        for (int c = 0; c < initial.columns; ++c) {
            grid->setItem(r, c, new QTableWidgetItem(initial.cells[r * initial.columns + c]));
        }
    }
    connect(rows, &QSpinBox::valueChanged, grid, &QTableWidget::setRowCount);
    connect(columns, &QSpinBox::valueChanged, grid, &QTableWidget::setColumnCount);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        DraftTable table{grid->columnCount(), width->value(), height->value(), {}};
        for (int r = 0; r < grid->rowCount(); ++r) {
            for (int c = 0; c < grid->columnCount(); ++c) {
                table.cells << cellText(*grid, r, c);
            }
        }
        item.table = std::move(table);
        if (!drawing::validItem(item)) {
            QMessageBox::warning(&dialog, tr("Drawing table"), tr("Table cells can contain at most 200 characters."));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (index >= 0) {
        editItem(index, item, tr("Edit table"));
        return;
    }
    statusBar()->showMessage(tr("Choose the table insertion point."));
    canvas_->pickPoint([this, item = std::move(item)](QPointF point) mutable {
        item.a = point;
        item.id = QUuid::createUuid();
        item.layer = layers_->currentIndex();
        if (layerEditable(draft_, item.layer)) {
            editItem(static_cast<int>(draft_.items.size()), std::move(item), tr("Insert table"));
        } else {
            statusBar()->showMessage(tr("Select a visible, unlocked layer to draw."), 4000);
        }
    });
}

}  // namespace graph3d
