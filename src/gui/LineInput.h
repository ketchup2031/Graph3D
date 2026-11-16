#pragma once

#include <QPointF>
#include <QWidget>

#include <optional>

class QDoubleSpinBox;
class QPushButton;

namespace graph3d {

class LineInput final : public QWidget {
    Q_OBJECT

public:
    explicit LineInput(QWidget* parent);
    void begin();
    void reset();
    void track(QPointF delta);
    std::optional<QPointF> exactDelta() const;

signals:
    void previewChanged();
    void lineRequested(QPointF delta);

private:
    QPointF delta() const;

    QDoubleSpinBox* length_;
    QDoubleSpinBox* angle_;
    QPushButton* add_;
    bool exact_ = false;
};

}  // namespace graph3d
