#pragma once

#include "drawing/Projection.h"

#include <QDialog>

namespace graph3d {

class DrawingDialog final : public QDialog {
public:
    DrawingDialog(drawing::Projections projections, const QString& object, int id, QWidget* parent);

private:
    class Preview;
    void exportPdf();
    Preview* preview_;
};

}  // namespace graph3d
