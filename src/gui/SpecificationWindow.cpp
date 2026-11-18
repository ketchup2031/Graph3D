#include "gui/SpecificationWindow.h"
#include "gui/DocumentWindow.h"
#include "gui/InterfaceLanguage.h"
#include "gui/UserPreferences.h"
#include "io/FileIO.h"
#include "io/DraftIO.h"
#include "io/SpecificationIO.h"

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace graph3d {
namespace {
class Edit final : public QUndoCommand {
public:
    using Change = std::function<void(const drawing::SpecificationDocument&)>;
    Edit(drawing::SpecificationDocument previous, drawing::SpecificationDocument next, Change apply)
        : previous_(std::move(previous)), next_(std::move(next)), apply_(std::move(apply)) {}
    void redo() override { apply_(next_); }
    void undo() override { apply_(previous_); }
private:
    drawing::SpecificationDocument previous_, next_;
    Change apply_;
};
QFileDialog::Options fileOptions()
{
    return darkThemeEnabled() || russianInterface() ? QFileDialog::DontUseNativeDialog : QFileDialog::Options{};
}
}

SpecificationWindow::SpecificationWindow(DocumentWindow* host) : QMainWindow(host), host_(host)
{
    setObjectName(QStringLiteral("specificationDocument"));
    auto* page = new QWidget(this);
    setCentralWidget(page);
    auto* layout = new QVBoxLayout(page);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    title_ = new QLineEdit(page);
    designation_ = new QLineEdit(page);
    title_->setMaxLength(1000);
    designation_->setMaxLength(1000);
    form->addRow(tr("Name"), title_);
    form->addRow(tr("Designation"), designation_);
    links_ = new QLabel(page);
    links_->setWordWrap(true);
    links_->setTextFormat(Qt::PlainText);
    layout->addWidget(links_);
    table_ = new QTableWidget(0, 8, page);
    table_->setObjectName(QStringLiteral("specificationDocumentRows"));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setHorizontalHeaderLabels({tr("Pos."), tr("Section"), tr("Designation"), tr("Name"),
        tr("Material"), tr("Qty."), tr("Note"), tr("Origin")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    for (int column : {0, 5, 7}) { table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents); }
    layout->addWidget(table_);
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("Save specification"), QKeySequence::Save, this, [this] { save(); });
    file->addAction(tr("Save specification as…"), QKeySequence::SaveAs, this, [this] { save(true); });
    file->addAction(tr("Export specification PDF…"), this, [this] { exportFile(true); });
    file->addAction(tr("Export specification CSV…"), this, [this] { exportFile(false); });
    if (host_) {
        file->addSeparator();
        file->addAction(tr("Open document…"), this, [this] { host_->browse(); });
        file->addAction(tr("Start page"), this, [this] { host_->showHome(); });
    }
    auto* edit = menuBar()->addMenu(tr("&Edit"));
    auto* undo = undo_.createUndoAction(this, tr("Undo"));
    auto* redo = undo_.createRedoAction(this, tr("Redo"));
    undo->setShortcut(QKeySequence::Undo);
    redo->setShortcut(QKeySequence::Redo);
    edit->addAction(undo);
    edit->addAction(redo);
    edit->addAction(tr("Add manual row"), this, &SpecificationWindow::addManual)->setObjectName(QStringLiteral("addManualSpecRow"));
    edit->addAction(tr("Remove manual row"), this, &SpecificationWindow::removeRow);
    auto* link = menuBar()->addMenu(tr("Links"));
    link->addAction(tr("Link assembly and variant…"), this, &SpecificationWindow::chooseAssembly);
    link->addAction(tr("Link drawing sheet…"), this, &SpecificationWindow::chooseDrawing);
    link->addAction(tr("Refresh from assembly"), this, &SpecificationWindow::refresh)->setObjectName(QStringLiteral("refreshSpecification"));
    link->addAction(tr("Compare model, table and drawing…"), this, &SpecificationWindow::audit);
    connect(table_, &QTableWidget::cellChanged, this, &SpecificationWindow::editCell);
    connect(title_, &QLineEdit::editingFinished, this, &SpecificationWindow::editTitle);
    connect(designation_, &QLineEdit::editingFinished, this, &SpecificationWindow::editTitle);
    connect(&undo_, &QUndoStack::cleanChanged, this, [this] { updateTitle(); });
    render();
    updateTitle();
}

void SpecificationWindow::issue(const QString& title, const std::exception& error)
{
    QMessageBox::warning(this, title, localizedDiagnostic(error.what()));
}

bool SpecificationWindow::open(const QString& path)
{
    try {
        auto spec = io::openSpecification(path);
        path_ = QFileInfo(path).absoluteFilePath();
        loadDocument(std::move(spec));
        return true;
    } catch (const std::exception& error) {
        issue(tr("Open specification"), error);
        return false;
    }
}

void SpecificationWindow::loadDocument(drawing::SpecificationDocument spec)
{
    spec_ = std::move(spec);
    undo_.clear();
    render();
    updateTitle();
}

void SpecificationWindow::setDocument(drawing::SpecificationDocument spec) { replace(std::move(spec)); }

bool SpecificationWindow::save(bool choosePath)
{
    auto name = path_;
    if (choosePath || name.isEmpty()) {
        name = QFileDialog::getSaveFileName(this, tr("Save specification"), name,
            tr("Graph3D specifications (*.g3spec)"), nullptr, fileOptions());
        if (name.isEmpty()) { return false; }
        if (QFileInfo(name).suffix().isEmpty()) { name += QStringLiteral(".g3spec"); }
    }
    if (!name.endsWith(".g3spec", Qt::CaseInsensitive)) {
        statusBar()->showMessage(tr("Use the .g3spec extension."), 5000);
        return false;
    }
    if (host_ && !host_->canSaveAs(this, name)) { return false; }
    try {
        editTitle();
        io::saveSpecification(name, spec_);
        path_ = QFileInfo(name).absoluteFilePath();
        undo_.setClean();
        updateTitle();
        if (host_) { host_->rememberProject(path_); }
        return true;
    } catch (const std::exception& error) {
        issue(tr("Save specification"), error);
        return false;
    }
}

bool SpecificationWindow::confirmSave()
{
    if (undo_.isClean()) { return true; }
    const auto action = QMessageBox::question(this, tr("Save specification"), tr("Save changes to the specification?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return action == QMessageBox::Discard || (action == QMessageBox::Save && save());
}

void SpecificationWindow::replace(drawing::SpecificationDocument spec)
{
    drawing::validateSpecification(spec);
    if (spec == spec_) { return; }
    undo_.push(new Edit(spec_, std::move(spec), [this](const auto& newState) {
        spec_ = newState;
        render();
        updateTitle();
    }));
}

void SpecificationWindow::updateTitle()
{
    setWindowModified(!undo_.isClean());
    setWindowTitle(tr("%1[*] — Graph3D Specification").arg(path_.isEmpty() ? tr("Untitled specification") : QFileInfo(path_).fileName()));
    if (host_) { host_->documentChanged(this); }
}

void SpecificationWindow::render()
{
    title_->setText(spec_.title);
    designation_->setText(spec_.designation);
    const auto variant = !spec_.variant ? tr("Use the model's active variant")
        : spec_.variant->isEmpty() ? tr("Base values") : *spec_.variant;
    auto summary = spec_.sourcePath.isEmpty() ? tr("No assembly linked.")
        : tr("Assembly: %1 #%2 · %3").arg(QFileInfo(spec_.sourcePath).fileName()).arg(spec_.assemblyId).arg(variant);
    if (!spec_.drawingPath.isEmpty()) {
        summary += u'\n' + tr("Drawing: %1 · sheet %2").arg(QFileInfo(spec_.drawingPath).fileName()).arg(spec_.drawingSheet + 1);
    }
    links_->setText(summary);
    links_->setToolTip(spec_.sourcePath + u'\n' + spec_.drawingPath);
    const QSignalBlocker block(table_);
    table_->clearContents();
    table_->setRowCount(static_cast<int>(spec_.rows.size()));
    for (int r = 0; r < table_->rowCount(); ++r) {
        const auto& row = spec_.rows[r];
        const bool manual = row.key.isEmpty();
        const QStringList cells{QString::number(row.position), row.section, row.designation, row.name,
            row.material, QString::number(row.quantity), row.note, manual ? tr("Manual") : tr("Model")};
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = new QTableWidgetItem(cells[c]);
            if (c == 7 || (c == 0 && !manual)) { item->setFlags(item->flags() & ~Qt::ItemIsEditable); }
            table_->setItem(r, c, item);
        }
    }
}

void SpecificationWindow::editTitle()
{
    auto next = spec_;
    next.title = title_->text().trimmed();
    next.designation = designation_->text().trimmed();
    replace(std::move(next));
}

void SpecificationWindow::editCell(int row, int column)
{
    auto next = spec_;
    auto& value = next.rows.at(row);
    const auto text = table_->item(row, column)->text().trimmed();
    bool ok = false;
    switch (column) {
    case 0: {
        const auto number = text.toInt(&ok);
        if (ok && number > 0 && number < 1000000 && value.key.isEmpty()) {
            value.position = number; next.nextPosition = std::max(next.nextPosition, number + 1);
        } else { render(); return; }
        break;
    }
    case 1: value.section = text; break;
    case 2: value.designation = text; break;
    case 3: value.name = text; break;
    case 4: value.material = text; break;
    case 5: value.quantity = text.toInt(&ok); if (!ok) { render(); return; } break;
    case 6: value.note = text; break;
    default: return;
    }
    try {
        drawing::validateSpecification(next);
        if (next != spec_) {
            // The editor is already showing the changed cell; redraw on undo/redo.
            undo_.push(new Edit(spec_, std::move(next), [this](const auto& state) {
                spec_ = state;
                QMetaObject::invokeMethod(this, [this] { render(); }, Qt::QueuedConnection);
                updateTitle();
            }));
        }
    } catch (const std::exception& error) {
        render();
        statusBar()->showMessage(localizedDiagnostic(error.what()), 7000);
    }
}

void SpecificationWindow::addManual()
{
    auto next = spec_;
    next.rows.push_back({{}, tr("Other items"), {}, tr("New item"), {}, {}, next.nextPosition++, 1});
    try { replace(std::move(next)); } catch (const std::exception& error) { issue(tr("Add manual row"), error); }
}

void SpecificationWindow::removeRow()
{
    const int index = table_->currentRow();
    if (index < 0) { return; }
    if (!spec_.rows[index].key.isEmpty()) {
        statusBar()->showMessage(tr("Model rows are removed by updating the assembly."), 5000);
        return;
    }
    auto next = spec_;
    next.rows.erase(next.rows.begin() + index);
    replace(std::move(next));
}

void SpecificationWindow::chooseAssembly()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Select source assembly"), spec_.sourcePath,
        tr("Graph3D projects (*.graph3d)"), nullptr, fileOptions());
    if (path.isEmpty()) { return; }
    try {
        const auto source = io::openModelSource(path);
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Link assembly and variant"));
        QFormLayout form(&dialog);
        QComboBox variants, assemblies;
        variants.addItems({tr("Use the model's active variant"), tr("Base values")});
        std::vector<std::optional<QString>> choices{std::nullopt, QString{}};
        for (const auto& [name, values] : source.document.configuration().variants) {
            variants.addItem(QString::fromStdString(name));
            choices.push_back(QString::fromStdString(name));
        }
        form.addRow(tr("Variant"), &variants);
        form.addRow(tr("Assembly"), &assemblies);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form.addRow(&buttons);
        const auto list = [&] {
            assemblies.clear();
            auto model = source.document;
            try {
                if (const auto& variant = choices.at(variants.currentIndex())) { model.selectVariant(variant->toStdString()); }
                for (const auto& item : model.objects()) {
                    const auto* linked = std::get_if<document::LinkedComponent>(&item.parameters);
                    if (io::canProjectBody(item) && (std::holds_alternative<document::Assembly>(item.parameters)
                        || (linked && !linked->record.children.empty()))) {
                        const auto record = model.componentRecord(item.id);
                        assemblies.addItem(tr("Assembly #%1 · %2").arg(item.id).arg(QString::fromStdString(record.properties.name)), item.id);
                    }
                }
            } catch (const std::exception& error) {
                statusBar()->showMessage(localizedDiagnostic(error.what()), 7000);
            }
            buttons.button(QDialogButtonBox::Ok)->setEnabled(assemblies.count() > 0);
            if (const int index = assemblies.findData(spec_.assemblyId); index >= 0) { assemblies.setCurrentIndex(index); }
        };
        connect(&variants, &QComboBox::currentIndexChanged, &dialog, list);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (const auto old = std::ranges::find(choices, spec_.variant); old != choices.end()) {
            variants.setCurrentIndex(static_cast<int>(old - choices.begin()));
        }
        list();
        if (dialog.exec() != QDialog::Accepted || assemblies.currentIndex() < 0) { return; }
        auto next = spec_;
        const auto id = assemblies.currentData().toInt();
        if (next.sourcePath != source.path || next.assemblyId != id) {
            std::erase_if(next.rows, [](const auto& row) { return !row.key.isEmpty(); });
            next.history.clear();
        }
        next.sourcePath = source.path;
        next.assemblyId = id;
        next.variant = choices.at(variants.currentIndex());
        if (next.sourcePath != spec_.sourcePath || next.assemblyId != spec_.assemblyId || next.variant != spec_.variant) {
            next.drawingPath.clear();
            next.drawingSheet = 0;
        }
        drawing::mergeSpecification(next, *io::readSpecificationAssembly(next));
        replace(std::move(next));
    } catch (const std::exception& error) { issue(tr("Link assembly and variant"), error); }
}

void SpecificationWindow::chooseDrawing()
{
    if (spec_.sourcePath.isEmpty()) {
        statusBar()->showMessage(tr("Link a source assembly first."), 5000);
        return;
    }
    const auto path = QFileDialog::getOpenFileName(this, tr("Link drawing sheet"), spec_.drawingPath,
        tr("Graph3D drawings (*.g3draw)"), nullptr, fileOptions());
    if (path.isEmpty()) { return; }
    try {
        auto draft = io::openDraft(path);
        QStringList names;
        std::vector<int> indexes;
        for (int i = 0; i <= static_cast<int>(draft.additionalSheets.size()); ++i) {
            const auto* sheet = io::specificationSheet(draft, i);
            if (sheet->modelViews && io::specificationMatches(spec_, *sheet->modelViews)) {
                names << tr("Sheet %1").arg(i + 1);
                indexes.push_back(i);
            }
        }
        if (names.isEmpty()) { throw std::invalid_argument("The drawing has no sheet for this assembly and variant."); }
        bool accepted = true;
        const auto choice = names.size() == 1 ? names.front() : QInputDialog::getItem(this, tr("Link drawing sheet"),
            tr("Drawing sheet"), names, 0, false, &accepted);
        if (accepted) {
            auto next = spec_;
            next.drawingPath = QFileInfo(path).absoluteFilePath();
            next.drawingSheet = indexes.at(names.indexOf(choice));
            replace(std::move(next));
        }
    } catch (const std::exception& error) { issue(tr("Link drawing sheet"), error); }
}

void SpecificationWindow::refresh()
{
    try {
        auto next = spec_;
        drawing::mergeSpecification(next, *io::readSpecificationAssembly(next));
        replace(std::move(next));
        statusBar()->showMessage(tr("Specification updated from the assembly."), 5000);
    } catch (const std::exception& error) { issue(tr("Refresh from assembly"), error); }
}

void SpecificationWindow::audit()
{
    QStringList issues;
    std::shared_ptr<const drawing::AssemblyDrawing> model;
    std::optional<drawing::Draft> drawingFile;
    const drawing::ModelViews* views = nullptr;
    try { model = io::readSpecificationAssembly(spec_); }
    catch (const std::exception& error) { issues << localizedDiagnostic(error.what()); }
    if (spec_.drawingPath.isEmpty()) { issues << tr("No drawing linked."); }
    else {
        try {
            drawingFile = io::openDraft(spec_.drawingPath);
            const auto* sheet = io::specificationSheet(*drawingFile, spec_.drawingSheet);
            if (sheet && sheet->modelViews && io::specificationMatches(spec_, *sheet->modelViews)) {
                views = &*sheet->modelViews;
                if (io::currentModelDigest(spec_.sourcePath) != views->sourceDigest) {
                    issues << tr("The drawing was built from an older version of the model.");
                }
                if (!path_.isEmpty() && (views->specificationPath.isEmpty()
                    || QFileInfo(views->specificationPath) != QFileInfo(path_))) {
                    issues << tr("The drawing sheet is not linked back to this specification.");
                }
            } else { issues << tr("The linked drawing sheet uses another assembly or variant."); }
        } catch (const std::exception& error) { issues << localizedDiagnostic(error.what()); }
    }
    issues << drawing::auditSpecification(spec_, model.get(), views);
    QMessageBox message(QMessageBox::Information, tr("Specification comparison"), issues.isEmpty()
        ? tr("Model, table and drawing positions agree.") : tr("Discrepancies found: %1").arg(issues.size()), QMessageBox::Ok, this);
    message.setTextFormat(Qt::PlainText);
    if (!issues.isEmpty()) { message.setDetailedText(issues.join(u'\n')); }
    message.exec();
}

void SpecificationWindow::exportFile(bool pdf)
{
    const auto suffix = pdf ? QStringLiteral(".pdf") : QStringLiteral(".csv");
    const auto title = pdf ? tr("Export specification PDF") : tr("Export specification CSV");
    auto path = QFileDialog::getSaveFileName(this, title, {},
        pdf ? tr("PDF files (*.pdf)") : tr("CSV files (*.csv)"), nullptr, fileOptions());
    if (path.isEmpty()) { return; }
    if (QFileInfo(path).suffix().isEmpty()) { path += suffix; }
    try {
        if (!path.endsWith(suffix, Qt::CaseInsensitive)) { throw std::invalid_argument("Use the extension for the selected export format."); }
        if (pdf) { drawing::exportSpecificationPdf(path, spec_); }
        else { io::writeFile(path, drawing::specificationCsv(spec_)); }
        statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 5000);
    } catch (const std::exception& error) { issue(title, error); }
}
}  // namespace graph3d
