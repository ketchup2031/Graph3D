#include "gui/PropertyPanel.h"

#include "document/Document.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>

namespace graph3d {

PropertyPanel::PropertyPanel(QWidget* parent)
    : QWidget(parent), form_(new QFormLayout(this)), object_(new QLabel(this)),
      details_(new QLabel(this)), state_(new QLabel(this)), distance_(new QDoubleSpinBox(this)),
      axis_(new QComboBox(this)), offset_(new QDoubleSpinBox(this)), angle_(new QDoubleSpinBox(this)),
      apply_(new QPushButton(tr("Apply"), this))
{
    setObjectName(QStringLiteral("propertyPanel"));
    object_->setObjectName(QStringLiteral("propertyObject"));
    state_->setObjectName(QStringLiteral("propertyState"));
    distance_->setObjectName(QStringLiteral("propertyDistance"));
    axis_->setObjectName(QStringLiteral("propertyAxis"));
    offset_->setObjectName(QStringLiteral("propertyOffset"));
    angle_->setObjectName(QStringLiteral("propertyAngle"));
    apply_->setObjectName(QStringLiteral("applyProperties"));
    for (auto* label : {object_, details_, state_}) {
        label->setWordWrap(true);
    }
    state_->setTextFormat(Qt::PlainText);
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form_->addRow(object_);
    form_->addRow(details_);
    form_->addRow(state_);
    for (auto* number : {distance_, offset_}) {
        number->setRange(-100000, 100000);
        number->setSuffix(tr(" mm"));
    }
    axis_->addItems({tr("U (horizontal)"), tr("V (vertical)")});
    angle_->setRange(0.01, 360);
    angle_->setSuffix(QStringLiteral("°"));
    form_->addRow(tr("Distance"), distance_);
    form_->addRow(tr("Axis"), axis_);
    form_->addRow(tr("Offset"), offset_);
    form_->addRow(tr("Angle"), angle_);
    form_->addRow(apply_);
    connect(apply_, &QPushButton::clicked, this, [this] {
        if (id_ && form_->isRowVisible(distance_)) {
            emit extrudeEdited(id_, distance_->value());
        } else if (id_ && form_->isRowVisible(axis_)) {
            const auto axis = axis_->currentIndex() == 0 ? geometry::Axis::U : geometry::Axis::V;
            emit revolveEdited(id_, axis, offset_->value(), angle_->value());
        }
    });
    showObject(nullptr, {}, {});
}

void PropertyPanel::showObject(const document::Object* object, const QString& name, const QString& details)
{
    id_ = object ? object->id : 0;
    object_->setText(object ? name : tr("Select an object in the model tree."));
    details_->setText(details);
    QString status;
    if (object) {
        const auto error = QString::fromStdString(object->error);
        switch (object->status) {
        case document::Status::Ready: status = tr("Ready"); break;
        case document::Status::Failed: status = tr("Invalid: %1").arg(error); break;
        case document::Status::Blocked: status = tr("Blocked: %1").arg(error); break;
        }
    }
    state_->setText(status);
    form_->setRowVisible(details_, !details.isEmpty());
    form_->setRowVisible(state_, !status.isEmpty());
    const auto* extrusion = object ? std::get_if<document::Extrude>(&object->parameters) : nullptr;
    const auto* revolution = object ? std::get_if<document::Revolve>(&object->parameters) : nullptr;
    form_->setRowVisible(distance_, extrusion != nullptr);
    for (auto* field : {static_cast<QWidget*>(axis_), static_cast<QWidget*>(offset_), static_cast<QWidget*>(angle_)}) {
        form_->setRowVisible(field, revolution != nullptr);
    }
    form_->setRowVisible(apply_, extrusion || revolution);
    if (extrusion) {
        distance_->setValue(extrusion->distance);
    }
    if (revolution) {
        axis_->setCurrentIndex(revolution->axis == geometry::Axis::U ? 0 : 1);
        offset_->setValue(revolution->offset);
        angle_->setValue(revolution->angle);
    }
}

}  // namespace graph3d
