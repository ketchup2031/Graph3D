#pragma once

#include "geometry/Sketch.h"

#include <QWidget>

class QLabel;

namespace graph3d {

class LineInput;

class SketchEditor final : public QWidget {
    Q_OBJECT

public:
    explicit SketchEditor(QWidget* parent = nullptr);
    void begin(geometry::Plane plane);
    const geometry::Sketch& sketch() const;
    void showError(const QString& message);

signals:
    void finishRequested();
    void cancelRequested();

private:
    class Canvas;
    LineInput* lineInput_;
    Canvas* canvas_;
    QLabel* planeLabel_;
    QLabel* feedback_;
};

}  // namespace graph3d
