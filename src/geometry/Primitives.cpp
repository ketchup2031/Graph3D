#include "geometry/Primitives.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <stdexcept>

namespace graph3d::geometry {

TopoDS_Shape makePrimitive(Primitive primitive, double x, double y)
{
    constexpr double size = 100.0;
    constexpr double radius = size / 2.0;
    TopoDS_Shape shape;
    switch (primitive) {
    case Primitive::Cube:
        shape = BRepPrimAPI_MakeBox(gp_Pnt(x, y, 0.0), size, size, size).Shape();
        break;
    case Primitive::Cylinder:
        shape = BRepPrimAPI_MakeCylinder(
                    gp_Ax2(gp_Pnt(x + radius, y + radius, 0.0), gp_Dir(0.0, 0.0, 1.0)),
                    radius, size).Shape();
        break;
    case Primitive::Sphere:
        shape = BRepPrimAPI_MakeSphere(gp_Pnt(x + radius, y + radius, radius), radius).Shape();
        break;
    }
    if (shape.IsNull() || shape.ShapeType() != TopAbs_SOLID || !BRepCheck_Analyzer(shape).IsValid()) {
        throw std::runtime_error("OCCT did not produce a valid solid.");
    }
    return shape;
}

}  // namespace graph3d::geometry
