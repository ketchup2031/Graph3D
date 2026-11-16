#pragma once

#include "drawing/Projection.h"

#include <QDialog>
#include <TopoDS_Shape.hxx>

namespace graph3d {

class DrawingDialog final : public QDialog {
public:
    DrawingDialog(const TopoDS_Shape& body, drawing::Projections projections,
                  const QString& object, int id, QWidget* parent);

private:
    class Preview;
    void exportPdf();
    TopoDS_Shape body_;
    Preview* preview_;
};

}  // namespace graph3d
