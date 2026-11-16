#include "gui/TransformEditor.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>

namespace graph3d {

TransformEditor::TransformEditor(QWidget* parent)
    : QWidget(parent), form_(new QFormLayout(this)), axis_(new QComboBox(this)),
      plane_(new QComboBox(this)), count_(new QSpinBox(this)),
      spacing_(new QDoubleSpinBox(this)), sweep_(new QDoubleSpinBox(this)),
      offset_(new QDoubleSpinBox(this))
{
    form_->setContentsMargins(0, 0, 0, 0);
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    axis_->setObjectName(QStringLiteral("transformAxis"));
    plane_->setObjectName(QStringLiteral("transformPlane"));
    count_->setObjectName(QStringLiteral("transformCount"));
    spacing_->setObjectName(QStringLiteral("transformSpacing"));
    sweep_->setObjectName(QStringLiteral("transformSweep"));
    offset_->setObjectName(QStringLiteral("transformOffset"));
    axis_->addItems({QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")});
    plane_->addItems({QStringLiteral("XY"), QStringLiteral("XZ"), QStringLiteral("YZ")});
    count_->setRange(2, geometry::maxArrayInstances);
    count_->setToolTip(tr("Includes the source body."));
    for (auto* distance : {spacing_, offset_}) {
        distance->setDecimals(4);
        distance->setRange(-1000000, 1000000);
        distance->setSuffix(tr(" mm"));
    }
    sweep_->setDecimals(4);
    sweep_->setRange(0.0001, 360);
    sweep_->setSuffix(QStringLiteral("°"));
    sweep_->setToolTip(tr("Total angle from the first to last instance. At 360°, instances are evenly spaced around the full circle."));
    form_->addRow(tr("Axis"), axis_);
    form_->addRow(tr("Plane"), plane_);
    form_->addRow(tr("Instances"), count_);
    form_->addRow(tr("Spacing"), spacing_);
    form_->addRow(tr("Sweep"), sweep_);
    form_->addRow(tr("Plane offset"), offset_);
    setParameters(current_);
}

void TransformEditor::setParameters(const geometry::BodyTransform& parameters)
{
    current_ = parameters;
    const auto* linear = std::get_if<geometry::LinearArray>(&current_);
    const auto* polar = std::get_if<geometry::PolarArray>(&current_);
    const auto* mirror = std::get_if<geometry::Mirror>(&current_);
    form_->setRowVisible(axis_, !mirror);
    form_->setRowVisible(plane_, mirror != nullptr);
    form_->setRowVisible(count_, !mirror);
    form_->setRowVisible(spacing_, linear != nullptr);
    form_->setRowVisible(sweep_, polar != nullptr);
    form_->setRowVisible(offset_, mirror != nullptr);
    auto* axisLabel = qobject_cast<QLabel*>(form_->labelForField(axis_));
    axisLabel->setText(polar ? tr("Axis through origin") : tr("Axis"));
    if (linear) {
        axis_->setCurrentIndex(static_cast<int>(linear->axis));
        count_->setValue(linear->count);
        spacing_->setValue(linear->spacing);
    } else if (polar) {
        axis_->setCurrentIndex(static_cast<int>(polar->axis));
        count_->setValue(polar->count);
        sweep_->setValue(polar->sweep);
    } else {
        plane_->setCurrentIndex(static_cast<int>(mirror->plane));
        offset_->setValue(mirror->offset);
    }
}

geometry::BodyTransform TransformEditor::parameters() const
{
    const auto axis = static_cast<geometry::ModelAxis>(axis_->currentIndex());
    if (std::holds_alternative<geometry::LinearArray>(current_)) {
        return geometry::LinearArray{axis, count_->value(), spacing_->value()};
    }
    if (std::holds_alternative<geometry::PolarArray>(current_)) {
        return geometry::PolarArray{axis, count_->value(), sweep_->value()};
    }
    return geometry::Mirror{static_cast<geometry::Plane>(plane_->currentIndex()), offset_->value()};
}

}  // namespace graph3d
