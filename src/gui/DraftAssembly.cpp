#include "gui/DraftWindow.h"

#include "drawing/Specification.h"
#include "gui/DraftCanvas.h"
#include "gui/DocumentWindow.h"
#include "gui/InterfaceLanguage.h"
#include "gui/UserPreferences.h"
#include "io/SpecificationIO.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QStatusBar>
#include <QTableWidget>
#include <QVBoxLayout>

namespace graph3d {

void DraftWindow::createSpecification()
{
    if (!host_ || !draft_.modelViews || !draft_.modelViews->assembly) { return; }
    const auto& model = *draft_.modelViews;
    try {
        if (!model.specificationPath.isEmpty()) {
            host_->openDocument(model.specificationPath);
            return;
        }
        drawing::SpecificationDocument spec;
        spec.sourcePath = model.sourcePath;
        spec.assemblyId = model.bodyId;
        spec.variant = model.variant;
        spec.drawingPath = path_;
        spec.drawingSheet = path_.isEmpty() ? 0 : activeSheet_;
        spec.nextPosition = model.assembly->nextPosition;
        spec.history = model.assembly->positionHistory;
        for (const auto& row : model.assembly->rows) { spec.history[row.key] = row.position; }
        drawing::mergeSpecification(spec, *io::readSpecificationAssembly(spec));
        host_->createSpecification(std::move(spec));
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Create specification document"), localizedDiagnostic(error.what()));
    }
}

void DraftWindow::linkSpecification()
{
    if (!draft_.modelViews || !draft_.modelViews->assembly) { return; }
    const auto path = QFileDialog::getOpenFileName(this, tr("Link specification document"), draft_.modelViews->specificationPath,
        tr("Graph3D specifications (*.g3spec)"), nullptr,
        darkThemeEnabled() || russianInterface() ? QFileDialog::DontUseNativeDialog : QFileDialog::Options{});
    if (path.isEmpty()) { return; }
    try {
        auto model = *draft_.modelViews;
        io::attachSpecification(model, path);
        setModelViews(std::move(model), tr("Link specification document"));
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Link specification document"), localizedDiagnostic(error.what()));
    }
}

void DraftWindow::showSpecification()
{
    const auto& assembly = *draft_.modelViews->assembly;
    if (host_ && !draft_.modelViews->specificationPath.isEmpty()) {
        host_->openDocument(draft_.modelViews->specificationPath);
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("assemblySpecificationDialog"));
    dialog.setWindowTitle(tr("Assembly specification"));
    dialog.resize(720, 400);
    QVBoxLayout layout(&dialog);
    if (!assembly.name.isEmpty() || !assembly.designation.isEmpty()) {
        auto* title = new QLabel((assembly.designation + QStringLiteral("  ") + assembly.name).trimmed(), &dialog);
        title->setTextFormat(Qt::PlainText);
        layout.addWidget(title);
    }
    QTableWidget table(static_cast<int>(assembly.rows.size()), 6);
    table.setObjectName(QStringLiteral("specificationRows"));
    table.setEditTriggers(QAbstractItemView::NoEditTriggers);
    table.setSelectionBehavior(QAbstractItemView::SelectRows);
    QStringList headings;
    for (const auto& heading : drawing::specificationHeaders()) {
        headings << heading;
    }
    table.setHorizontalHeaderLabels(headings);
    table.verticalHeader()->hide();
    table.horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table.horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table.horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    for (int i = 0; i < table.rowCount(); ++i) {
        const auto cells = drawing::specificationCells(assembly.rows[i]);
        for (int col = 0; col < table.columnCount(); ++col) {
            auto* item = new QTableWidgetItem(cells[col]);
            item->setToolTip(cells[col].toHtmlEscaped());
            table.setItem(i, col, item);
        }
    }
    table.resizeRowsToContents();
    layout.addWidget(&table);
    QDialogButtonBox buttons(QDialogButtonBox::Close);
    layout.addWidget(&buttons);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void DraftWindow::placePosition()
{
    const auto& model = *draft_.modelViews;
    const auto& assembly = *model.assembly;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("chooseAssemblyPosition"));
    dialog.setWindowTitle(tr("Place position number"));
    QFormLayout form(&dialog);
    QComboBox positions, instances, views;
    positions.setObjectName(QStringLiteral("assemblyPosition"));
    instances.setObjectName(QStringLiteral("assemblyOccurrence"));
    views.setObjectName(QStringLiteral("assemblyPositionView"));
    for (const auto& row : assembly.rows) {
        positions.addItem(tr("%1 · %2").arg(row.position).arg(row.name));
    }
    const auto fillInstances = [&] {
        instances.clear();
        const auto& row = assembly.rows[positions.currentIndex()];
        for (std::size_t i = 0; i < row.instances.size(); ++i) {
            const auto p = drawing::positionOrigin(model, row, static_cast<int>(i), views.currentIndex());
            instances.addItem(tr("%1 · X %2, Y %3 mm").arg(i + 1).arg(p.x(), 0, 'f', 1).arg(p.y(), 0, 'f', 1));
        }
    };
    views.addItems({tr("Front"), tr("Right"), tr("Top")});
    form.addRow(tr("Position"), &positions);
    form.addRow(tr("View"), &views);
    form.addRow(tr("Component instance"), &instances);
    connect(&positions, &QComboBox::currentIndexChanged, &dialog, fillInstances);
    connect(&views, &QComboBox::currentIndexChanged, &dialog, fillInstances);
    fillInstances();
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto& row = assembly.rows[positions.currentIndex()];
    drawing::PositionCallout marker{row.position, instances.currentIndex(), views.currentIndex()};
    if (!row.componentSlots.empty()) { marker.componentSlot = row.componentSlots[marker.instance]; }
    const auto origin = drawing::positionOrigin(model, row, marker.instance, marker.view);
    const auto placement = model.layout[marker.view];
    const auto& geometry = model.geometry->views;
    const auto& view = *std::array{&geometry.front, &geometry.right, &geometry.top}[marker.view];
    const QRectF bounds(placement.center - QPointF(view.width, view.height) * placement.scale / 2,
                         QSizeF(view.width * placement.scale, view.height * placement.scale));
    statusBar()->showMessage(tr("Click the component in the selected view."));
    canvas_->pickPoint([this, marker, origin, scale = placement.scale, bounds](QPointF tip) mutable {
        if (!bounds.adjusted(-2, -2, 2, 2).contains(tip)) {
            statusBar()->showMessage(tr("The point is outside the selected view."), 5000);
            return;
        }
        marker.offset = (tip - origin) / scale;
        statusBar()->showMessage(tr("Click the position of the number."));
        canvas_->pickPoint([this, marker](QPointF label) mutable {
            auto model = *draft_.modelViews;
            auto assembly = std::make_shared<drawing::AssemblyDrawing>(*model.assembly);
            marker.label = label;
            assembly->callouts.push_back(marker);
            model.assembly = std::move(assembly);
            setModelViews(std::move(model), tr("Place position number"));
            statusBar()->clearMessage();
        });
    });
}

void DraftWindow::removePosition()
{
    auto model = *draft_.modelViews;
    auto assembly = std::make_shared<drawing::AssemblyDrawing>(*model.assembly);
    QStringList labels;
    for (std::size_t i = 0; i < assembly->callouts.size(); ++i) {
        const auto& marker = assembly->callouts[i];
        labels << tr("%1 · Position %2 · X %3, Y %4 mm").arg(i + 1).arg(marker.position)
            .arg(marker.label.x(), 0, 'f', 1).arg(marker.label.y(), 0, 'f', 1);
    }
    bool accepted = false;
    const auto choice = QInputDialog::getItem(this, tr("Remove position number"), tr("Callout"),
                                             labels, 0, false, &accepted);
    if (accepted) {
        assembly->callouts.erase(assembly->callouts.begin() + labels.indexOf(choice));
        model.assembly = std::move(assembly);
        setModelViews(std::move(model), tr("Remove position number"));
    }
}

void DraftWindow::setSpecificationExport(bool enabled)
{
    if (!draft_.modelViews || !draft_.modelViews->assembly
        || draft_.modelViews->assembly->includeSpecification == enabled) {
        return;
    }
    auto model = *draft_.modelViews;
    auto assembly = std::make_shared<drawing::AssemblyDrawing>(*model.assembly);
    assembly->includeSpecification = enabled;
    model.assembly = std::move(assembly);
    setModelViews(std::move(model), tr("Change specification export"));
}

}  // namespace graph3d
