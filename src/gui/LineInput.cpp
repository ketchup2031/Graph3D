#include "gui/LineInput.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>

#include <cmath>
#include <numbers>

namespace graph3d {

LineInput::LineInput(QWidget* parent)
    : QWidget(parent), length_(new QDoubleSpinBox(this)), angle_(new QDoubleSpinBox(this)),
      add_(new QPushButton(tr("Add line"), this))
{
    setObjectName(QStringLiteral("lineInput"));
    length_->setObjectName(QStringLiteral("lineLength"));
    angle_->setObjectName(QStringLiteral("lineAngle"));
    add_->setObjectName(QStringLiteral("addLine"));
    length_->setDecimals(3);
    length_->setRange(0, 10000);
    length_->setSuffix(tr(" mm"));
    angle_->setDecimals(3);
    angle_->setRange(-360, 360);
    angle_->setSuffix(QStringLiteral("°"));
    angle_->setToolTip(tr("Positive angles turn counterclockwise from the horizontal axis."));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    const auto field = [this, layout](const QString& name, QDoubleSpinBox* input) {
        auto* label = new QLabel(name, this);
        label->setBuddy(input);
        layout->addWidget(label);
        layout->addWidget(input);
        connect(input, &QDoubleSpinBox::valueChanged, this, [this] {
            exact_ = true;
            add_->setEnabled(length_->value() > 0);
            emit previewChanged();
        });
        connect(input->findChild<QLineEdit*>(), &QLineEdit::returnPressed, add_, &QPushButton::click);
    };
    field(tr("Length"), length_);
    field(tr("Angle"), angle_);
    layout->addWidget(add_);
    layout->addStretch();
    connect(add_, &QPushButton::clicked, this, [this] {
        emit lineRequested(delta());
    });
    reset();
}

void LineInput::begin()
{
    reset();
    setEnabled(true);
}

void LineInput::reset()
{
    exact_ = false;
    track({});
    setEnabled(false);
}

void LineInput::track(QPointF offset)
{
    if (!exact_) {
        const QSignalBlocker lengthSignals(length_);
        const QSignalBlocker angleSignals(angle_);
        length_->setValue(std::hypot(offset.x(), offset.y()));
        angle_->setValue(std::atan2(offset.y(), offset.x()) * 180 / std::numbers::pi);
        add_->setEnabled(length_->value() > 0);
    }
}

QPointF LineInput::delta() const
{
    const double radians = angle_->value() * std::numbers::pi / 180;
    const auto component = [&](double direction) {
        return std::abs(direction) < 1.e-12 ? 0.0 : length_->value() * direction;
    };
    return {component(std::cos(radians)), component(std::sin(radians))};
}

std::optional<QPointF> LineInput::exactDelta() const
{
    return exact_ ? std::optional(delta()) : std::nullopt;
}

}  // namespace graph3d
