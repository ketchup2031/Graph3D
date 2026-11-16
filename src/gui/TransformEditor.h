#pragma once

#include "geometry/BodyTransform.h"

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QSpinBox;

namespace graph3d {

class TransformEditor final : public QWidget {
public:
    explicit TransformEditor(QWidget* parent = nullptr);
    void setParameters(const geometry::BodyTransform& parameters);
    geometry::BodyTransform parameters() const;

private:
    QFormLayout* form_;
    QComboBox* axis_;
    QComboBox* plane_;
    QSpinBox* count_;
    QDoubleSpinBox* spacing_;
    QDoubleSpinBox* sweep_;
    QDoubleSpinBox* offset_;
    geometry::BodyTransform current_ = geometry::LinearArray{geometry::ModelAxis::X, 3, 100};
};

}  // namespace graph3d
