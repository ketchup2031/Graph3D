#pragma once

#include "geometry/BodyTransform.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QPushButton;

namespace graph3d::document { struct Object; }

namespace graph3d {

class TransformEditor;

class PropertyPanel final : public QWidget {
    Q_OBJECT

public:
    explicit PropertyPanel(QWidget* parent = nullptr);
    void showObject(const document::Object* object, const QString& name, const QString& details);

signals:
    void extrudeEdited(int id, double distance);
    void revolveEdited(int id, geometry::Axis axis, double offset, double angle);
    void holeEdited(int id, double diameter, double depth, bool through);
    void edgeTreatmentEdited(int id, double size);
    void transformEdited(int id, const geometry::BodyTransform& parameters);

private:
    QFormLayout* form_;
    QLabel* object_;
    QLabel* details_;
    QLabel* state_;
    QDoubleSpinBox* distance_;
    QComboBox* axis_;
    QDoubleSpinBox* offset_;
    QDoubleSpinBox* angle_;
    QDoubleSpinBox* diameter_;
    QDoubleSpinBox* depth_;
    QCheckBox* through_;
    QDoubleSpinBox* edgeSize_;
    TransformEditor* transform_;
    QPushButton* apply_;
    int id_ = 0;
};

}  // namespace graph3d
