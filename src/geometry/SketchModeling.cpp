#include "geometry/SketchModeling.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Wire.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace graph3d::geometry {
namespace {

gp_Pnt point(Plane plane, Point2 p)
{
    switch (plane) {
    case Plane::XY: return {p.u, p.v, 0};
    case Plane::XZ: return {p.u, 0, p.v};
    case Plane::YZ: return {0, p.u, p.v};
    }
    throw std::invalid_argument("Unknown sketch plane.");
}

gp_Dir localDirection(Plane plane, Axis axis)
{
    const auto position = point(plane, axis == Axis::U ? Point2{1, 0} : Point2{0, 1});
    return gp_Dir(position.XYZ());
}

gp_Dir normal(Plane plane)
{
    return localDirection(plane, Axis::U).Crossed(localDirection(plane, Axis::V));
}

bool finite(Point2 p)
{
    return std::isfinite(p.u) && std::isfinite(p.v);
}

void addLine(BRepBuilderAPI_MakeWire& wire, Plane plane, Line line)
{
    if (!finite(line.start) || !finite(line.end)) {
        throw std::invalid_argument("The profile has invalid coordinates.");
    }
    const auto start = point(plane, line.start);
    const auto end = point(plane, line.end);
    if (start.Distance(end) <= Precision::Confusion()) {
        throw std::invalid_argument("The profile has a zero-length edge.");
    }
    wire.Add(BRepBuilderAPI_MakeEdge(start, end).Edge());
    if (!wire.IsDone()) {
        throw std::invalid_argument("Connect each line to the previous line and close the contour.");
    }
}

TopoDS_Shape checkedSolid(const TopoDS_Shape& shape)
{
    if (shape.IsNull() || !TopExp_Explorer(shape, TopAbs_SOLID).More()
        || !BRepCheck_Analyzer(shape, true).IsValid()) {
        throw std::runtime_error("The operation did not produce a valid solid.");
    }
    GProp_GProps volume;
    BRepGProp::VolumeProperties(shape, volume);
    if (!(volume.Mass() > std::pow(Precision::Confusion(), 3))) {
        throw std::runtime_error("The operation did not produce material with positive volume.");
    }
    return shape;
}

template<class Algorithm>
TopoDS_Shape applyBoolean(const TopoDS_Shape& first, const TopoDS_Shape& second)
{
    TopTools_ListOfShape arguments;
    TopTools_ListOfShape tools;
    arguments.Append(first);
    tools.Append(second);
    Algorithm operation;
    operation.SetArguments(arguments);
    operation.SetTools(tools);
    operation.SetNonDestructive(true);
    operation.Build();
    if (!operation.IsDone() || operation.HasErrors()) {
        throw std::runtime_error("Open CASCADE could not complete the boolean operation.");
    }
    return checkedSolid(operation.Shape());
}

}  // namespace

TopoDS_Face makeProfile(const Sketch& sketch)
{
    if (sketch.entities.empty()) {
        throw std::invalid_argument("Draw a closed profile before finishing the sketch.");
    }
    BRepBuilderAPI_MakeWire wire;
    if (sketch.entities.size() == 1 && std::holds_alternative<Circle>(sketch.entities.front())) {
        const auto circle = std::get<Circle>(sketch.entities.front());
        if (!finite(circle.center) || !std::isfinite(circle.radius) || circle.radius <= Precision::Confusion()) {
            throw std::invalid_argument("The profile has an invalid circle.");
        }
        const gp_Ax2 frame(point(sketch.plane, circle.center), normal(sketch.plane));
        wire.Add(BRepBuilderAPI_MakeEdge(gp_Circ(frame, circle.radius)).Edge());
    } else if (sketch.entities.size() == 1 && std::holds_alternative<Rectangle>(sketch.entities.front())) {
        const auto rectangle = std::get<Rectangle>(sketch.entities.front());
        const std::array corners{rectangle.first, Point2{rectangle.opposite.u, rectangle.first.v},
                                 rectangle.opposite, Point2{rectangle.first.u, rectangle.opposite.v}};
        for (std::size_t i = 0; i < corners.size(); ++i) {
            addLine(wire, sketch.plane, {corners[i], corners[(i + 1) % corners.size()]});
        }
    } else {
        for (const auto& entity : sketch.entities) {
            const auto* line = std::get_if<Line>(&entity);
            if (!line) {
                throw std::invalid_argument("Use one rectangle, one circle, or one loop of connected lines.");
            }
            addLine(wire, sketch.plane, *line);
        }
    }
    if (!wire.IsDone() || !BRep_Tool::IsClosed(wire.Wire())) {
        throw std::invalid_argument("The profile is open. Connect the last point to the first.");
    }

    const gp_Pln plane(gp_Ax3(point(sketch.plane, {0, 0}), normal(sketch.plane),
                              localDirection(sketch.plane, Axis::U)));
    BRepBuilderAPI_MakeFace builder(plane, wire.Wire(), true);
    if (!builder.IsDone()) {
        throw std::invalid_argument("The profile does not enclose a planar face.");
    }
    const auto face = builder.Face();
    BRepCheck_Wire wireCheck(wire.Wire());
    TopoDS_Edge first;
    TopoDS_Edge second;
    GProp_GProps area;
    BRepGProp::SurfaceProperties(face, area);
    if (wireCheck.SelfIntersect(face, first, second) != BRepCheck_NoError
        || !BRepCheck_Analyzer(face, true).IsValid()
        || !(area.Mass() > std::pow(Precision::Confusion(), 2))) {
        throw std::invalid_argument("Use a single closed profile with no intersections or repeated edges.");
    }
    return face;
}

TopoDS_Shape extrude(const TopoDS_Face& face, Plane plane, double distance)
{
    if (!std::isfinite(distance) || std::abs(distance) <= Precision::Confusion()) {
        throw std::invalid_argument("Extrusion distance must be nonzero.");
    }
    BRepPrimAPI_MakePrism operation(face, gp_Vec(normal(plane)) * distance, true);
    return checkedSolid(operation.Shape());
}

TopoDS_Shape revolve(const TopoDS_Face& face, Plane plane, Axis axis, double offset, double degrees)
{
    if (!std::isfinite(offset) || !std::isfinite(degrees) || degrees <= 0 || degrees > 360) {
        throw std::invalid_argument("Enter a finite axis offset and an angle greater than zero and at most 360°.");
    }
    Bnd_Box bounds;
    BRepBndLib::AddOptimal(face, bounds, false, false);
    const auto lo = bounds.CornerMin();
    const auto hi = bounds.CornerMax();
    const Axis across = axis == Axis::U ? Axis::V : Axis::U;
    const gp_XYZ radial = localDirection(plane, across).XYZ();
    if (lo.XYZ().Dot(radial) < offset - Precision::Confusion()
        && hi.XYZ().Dot(radial) > offset + Precision::Confusion()) {
        throw std::invalid_argument("The rotation axis crosses the profile. Change the axis offset.");
    }
    const auto origin = point(plane, axis == Axis::U ? Point2{0, offset} : Point2{offset, 0});
    BRepPrimAPI_MakeRevol operation(face, gp_Ax1(origin, localDirection(plane, axis)),
                                    degrees * std::numbers::pi / 180.0, true);
    return checkedSolid(operation.Shape());
}

TopoDS_Shape booleanResult(const TopoDS_Shape& first, const TopoDS_Shape& second, BooleanOperation operation)
{
    if (first.IsNull() || second.IsNull() || first.IsSame(second)) {
        throw std::invalid_argument("Choose two different solids.");
    }
    switch (operation) {
    case BooleanOperation::Union: return applyBoolean<BRepAlgoAPI_Fuse>(first, second);
    case BooleanOperation::Difference: return applyBoolean<BRepAlgoAPI_Cut>(first, second);
    case BooleanOperation::Intersection: return applyBoolean<BRepAlgoAPI_Common>(first, second);
    }
    throw std::invalid_argument("Unknown boolean operation.");
}

}  // namespace graph3d::geometry
