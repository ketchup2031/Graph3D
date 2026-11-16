#include "gui/AssemblyDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace graph3d {
namespace {

enum Column { Body, Anchor, OffsetX, OffsetY, OffsetZ, RotateX, RotateY, RotateZ, ColumnCount };

}  // namespace

AssemblyDialog::AssemblyDialog(Sources sources, const document::Assembly* existing, QWidget* parent)
    : QDialog(parent), sources_(std::move(sources)), table_(new QTableWidget(0, ColumnCount, this)),
      editing_(existing != nullptr)
{
    setWindowTitle(editing_ ? tr("Edit component placement") : tr("Create assembly"));
    setObjectName(QStringLiteral("assemblyDialog"));
    resize(1050, 420);
    auto* layout = new QVBoxLayout(this);
    if (!editing_) {
        auto* form = new QFormLayout;
        auto* count = new QSpinBox(this);
        count->setObjectName(QStringLiteral("assemblyCount"));
        count->setRange(geometry::minAssemblyComponents, geometry::maxAssemblyComponents);
        form->addRow(tr("Components"), count);
        layout->addLayout(form);
        connect(count, &QSpinBox::valueChanged, this, [this](int size) {
            if (size < table_->rowCount()) {
                table_->setRowCount(size);
            }
            while (table_->rowCount() < size) {
                appendComponent({sources_.front().first, {}});
            }
        });
    }
    auto* help = new QLabel(tr("Choose the assembly origin or an earlier component as the frame for each placement."), this);
    help->setWordWrap(true);
    layout->addWidget(help);
    table_->setObjectName(QStringLiteral("assemblyComponents"));
    table_->setHorizontalHeaderLabels({tr("Source body"), tr("Relative to"), tr("X (mm)"), tr("Y (mm)"), tr("Z (mm)"),
                                       tr("X (°)"), tr("Y (°)"), tr("Z (°)")});
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    auto* header = table_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Stretch);
    header->setMinimumSectionSize(72);
    header->setSectionResizeMode(Body, QHeaderView::Interactive);
    header->setSectionResizeMode(Anchor, QHeaderView::Interactive);
    table_->setColumnWidth(Body, 185);
    table_->setColumnWidth(Anchor, 150);
    layout->addWidget(table_);
    if (editing_) {
        for (const auto& component : existing->components) {
            appendComponent(component);
        }
    } else {
        appendComponent({sources_.front().first, {}});
        const bool repeated = sources_.size() == 1;
        appendComponent({repeated ? sources_.front().first : sources_.at(1).first,
                         {0, {repeated ? 120.0 : 0.0, 0, 0}, {}}});
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void AssemblyDialog::appendComponent(const document::Component& component)
{
    const int row = table_->rowCount();
    table_->insertRow(row);
    auto* body = new QComboBox(table_);
    for (const auto& [id, name] : sources_) {
        body->addItem(name, id);
    }
    body->setCurrentIndex(body->findData(component.body));
    body->setEnabled(!editing_);
    table_->setCellWidget(row, Body, body);
    auto* anchor = new QComboBox(table_);
    anchor->addItem(tr("Assembly origin"));
    for (int i = 1; i <= row; ++i) {
        anchor->addItem(tr("Component %1").arg(i));
    }
    anchor->setCurrentIndex(component.placement.anchor >= 0 && component.placement.anchor <= row
                            ? component.placement.anchor : 0);
    table_->setCellWidget(row, Anchor, anchor);
    for (int axis = 0; axis < 3; ++axis) {
        auto* offset = new QDoubleSpinBox(table_);
        offset->setDecimals(4);
        offset->setRange(-geometry::maxComponentOffset, geometry::maxComponentOffset);
        offset->setValue(component.placement.offset[axis]);
        table_->setCellWidget(row, OffsetX + axis, offset);
        auto* angle = new QDoubleSpinBox(table_);
        angle->setDecimals(3);
        angle->setRange(-geometry::maxComponentAngle, geometry::maxComponentAngle);
        angle->setValue(component.placement.angles[axis]);
        table_->setCellWidget(row, RotateX + axis, angle);
    }
}

document::Assembly AssemblyDialog::assembly() const
{
    document::Assembly result;
    result.components.reserve(table_->rowCount());
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto* body = qobject_cast<QComboBox*>(table_->cellWidget(row, Body));
        const auto* anchor = qobject_cast<QComboBox*>(table_->cellWidget(row, Anchor));
        geometry::Placement placement;
        placement.anchor = anchor->currentIndex();
        for (int axis = 0; axis < 3; ++axis) {
            placement.offset[axis] = qobject_cast<QDoubleSpinBox*>(table_->cellWidget(row, OffsetX + axis))->value();
            placement.angles[axis] = qobject_cast<QDoubleSpinBox*>(table_->cellWidget(row, RotateX + axis))->value();
        }
        result.components.push_back({body->currentData().toInt(), placement});
    }
    return result;
}

}  // namespace graph3d
