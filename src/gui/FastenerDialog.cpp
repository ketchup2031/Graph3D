#include "gui/MainWindow.h"
#include "viewport/Viewport.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QStackedWidget>

namespace graph3d {
void MainWindow::configureFastener(int id)
{
    if (pages_->currentWidget() != viewport_ || pendingMachining_) { return; }
    geometry::Fastener initial;
    if (id) {
        const auto* object = document_.find(id);
        const auto* selected = object ? std::get_if<geometry::Fastener>(&object->parameters) : nullptr;
        if (!selected) { return; }
        initial = *selected;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("fastenerCatalogDialog"));
    dialog.setWindowTitle(tr("Fastener catalog"));
    QFormLayout form(&dialog);
    QComboBox kind, diameter, length;
    kind.setObjectName(QStringLiteral("catalogFastenerKind"));
    diameter.setObjectName(QStringLiteral("catalogFastenerDiameter"));
    length.setObjectName(QStringLiteral("catalogFastenerLength"));
    kind.addItems({tr("Hex bolt"), tr("Hex nut"), tr("Flat washer")});
    for (const auto& row : geometry::fastenerCatalog()) { diameter.addItem(QStringLiteral("M%1").arg(row.diameter), row.diameter); }
    kind.setCurrentIndex(static_cast<int>(initial.kind));
    diameter.setCurrentIndex(diameter.findData(initial.diameter));
    form.addRow(tr("Type"), &kind);
    form.addRow(tr("Nominal size"), &diameter);
    form.addRow(tr("Bolt length (mm)"), &length);
    QLabel description;
    description.setWordWrap(true);
    form.addRow(&description);
    const auto refresh = [&] {
        const bool bolt = kind.currentIndex() == static_cast<int>(geometry::FastenerKind::Bolt);
        const int selected = length.currentData().toInt();
        length.clear();
        for (const auto& row : geometry::fastenerCatalog()) {
            if (row.diameter == diameter.currentData().toInt()) {
                for (auto value : row.lengths) { length.addItem(QString::number(value), value); }
                description.setText(tr("Coarse metric pitch: %1 mm. Threads are represented without a helical profile.").arg(row.pitch));
                break;
            }
        }
        const int prior = length.findData(selected ? selected : initial.length);
        if (prior >= 0) { length.setCurrentIndex(prior); }
        length.setEnabled(bolt);
    };
    connect(&kind, &QComboBox::currentIndexChanged, &dialog, refresh);
    connect(&diameter, &QComboBox::currentIndexChanged, &dialog, refresh);
    refresh();
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) { return; }
    const auto type = static_cast<geometry::FastenerKind>(kind.currentIndex());
    const geometry::Fastener item{type, diameter.currentData().toInt(), type == geometry::FastenerKind::Bolt ? length.currentData().toInt() : 0};
    if (id) { editFeature(id, [=, this] { return document_.editFastener(id, item); }); }
    else { addFeature(tr("Insert catalog fastener"), [=, this] { return document_.addFastener(item); }); }
}
}  // namespace graph3d
