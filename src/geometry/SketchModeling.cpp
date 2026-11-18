#include "geometry/SketchModeling.h"
#include "geometry/SketchConstraints.h"
#include "geometry/SketchCurves.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Face.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepCheck_Wire.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <GProp_GProps.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_BezierCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <GeomConvert_CompCurveToBSplineCurve.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
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

gp_Ax3 cardinalFrame(Plane plane)
{
    return {point(plane, {0, 0}), normal(plane), localDirection(plane, Axis::U)};
}

gp_Pnt point(const gp_Ax3& frame, Point2 local)
{
    return frame.Location().Translated(gp_Vec(frame.XDirection()) * local.u + gp_Vec(frame.YDirection()) * local.v);
}

TopoDS_Edge lineEdge(const gp_Ax3& frame, Line line)
{
    if (!finite(line.start) || !finite(line.end)) {
        throw std::invalid_argument("The profile has invalid coordinates.");
    }
    const auto start = point(frame, line.start);
    const auto end = point(frame, line.end);
    if (start.Distance(end) <= Precision::Confusion()) {
        throw std::invalid_argument("The profile has a zero-length edge.");
    }
    return BRepBuilderAPI_MakeEdge(start, end).Edge();
}

TopoDS_Edge arcEdge(const gp_Ax3& frame, const Arc& arc)
{
    GC_MakeArcOfCircle curve(point(frame, arc.start), point(frame, arc.through), point(frame, arc.end));
    if (!curve.IsDone()) {
        throw std::invalid_argument("A sketch arc needs three distinct noncollinear points.");
    }
    return BRepBuilderAPI_MakeEdge(curve.Value()).Edge();
}

TopoDS_Edge splineEdge(const gp_Ax3& frame, const Spline& spline)
{
    GeomConvert_CompCurveToBSplineCurve joined;
    for (const auto& span : sketchSplineSpans(spline)) {
        TColgp_Array1OfPnt poles(1, 4);
        for (int i = 1; i <= 4; ++i) {
            poles.SetValue(i, point(frame, span[i - 1]));
        }
        if (!joined.Add(new Geom_BezierCurve(poles), Precision::Confusion(), true)) {
            throw std::invalid_argument("Could not connect the spline spans.");
        }
    }
    const auto curve = joined.BSplineCurve();
    if (curve.IsNull()) {
        throw std::invalid_argument("A sketch spline needs distinct interpolation points.");
    }
    return BRepBuilderAPI_MakeEdge(curve).Edge();
}

Handle(TopTools_HSequenceOfShape) sketchWires(const Sketch& sketch, const SketchContext& context)
{
    const auto solved = solveSketch(sketch, context.projections);
    if (!solved.projectionError.empty()) {
        throw std::invalid_argument(solved.projectionError);
    }
    if (!solved.conflicts.empty()) {
        throw std::invalid_argument("Sketch constraints conflict.");
    }
    const auto& frame = context.frame;
    Handle(TopTools_HSequenceOfShape) segments = new TopTools_HSequenceOfShape;
    Handle(TopTools_HSequenceOfShape) wires = new TopTools_HSequenceOfShape;
    for (int i = 0; i < static_cast<int>(solved.entities.size()); ++i) {
        if (isConstruction(sketch, i)) {
            continue;
        }
        const auto& entity = solved.entities[i];
        if (const auto* line = std::get_if<Line>(&entity)) {
            segments->Append(lineEdge(frame, *line));
        } else if (const auto* arc = std::get_if<Arc>(&entity)) {
            segments->Append(arcEdge(frame, *arc));
        } else if (const auto* spline = std::get_if<Spline>(&entity); spline && !spline->closed) {
            segments->Append(splineEdge(frame, *spline));
        } else {
            BRepBuilderAPI_MakeWire wire;
            if (const auto* circle = std::get_if<Circle>(&entity)) {
                const gp_Ax2 axes(point(frame, circle->center), frame.Direction(), frame.XDirection());
                wire.Add(BRepBuilderAPI_MakeEdge(gp_Circ(axes, circle->radius)).Edge());
            } else if (const auto* spline = std::get_if<Spline>(&entity)) {
                wire.Add(splineEdge(frame, *spline));
            } else {
                for (int side = 0; side < 4; ++side) {
                    wire.Add(lineEdge(frame, sketchSegment(entity, side)));
                }
            }
            if (!wire.IsDone()) {
                throw std::invalid_argument("The profile is open. Connect the last point to the first.");
            }
            wires->Append(wire.Wire());
        }
    }
    if (!segments->IsEmpty()) {
        Handle(TopTools_HSequenceOfShape) connected = new TopTools_HSequenceOfShape;
        ShapeAnalysis_FreeBounds::ConnectEdgesToWires(segments, Precision::Confusion(), false, connected);
        for (int i = 1; i <= connected->Length(); ++i) {
            wires->Append(connected->Value(i));
        }
    }
    return wires;
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

SketchContext sketchContext(const Sketch& sketch, const TopoDS_Shape& support)
{
    if (sketch.datumPlane) {
        throw std::invalid_argument("Select a valid construction plane.");
    }
    if (!sketch.support) {
        return {cardinalFrame(sketch.plane), {}};
    }
    const auto& reference = sketch.support->face;
    const auto face = resolveSketchFace(support, reference);
    const auto plane = BRepAdaptor_Surface(face).Plane();
    const gp_Dir outward(reference.outward[0], reference.outward[1], reference.outward[2]);
    const gp_Vec unit(outward);
    const auto offset = plane.Location().XYZ().Dot(outward.XYZ());
    const auto oldOffset = gp_XYZ(reference.center[0], reference.center[1], reference.center[2]).Dot(outward.XYZ());
    const gp_Pnt origin(outward.XYZ() * offset);
    const gp_Dir preferred = std::abs(outward.X()) < .9 ? gp_Dir(1, 0, 0) : gp_Dir(0, 1, 0);
    SketchContext result{gp_Ax3(origin, outward, preferred), {}};
    const auto& frame = result.frame;

    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(face, TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
        const auto edge = TopoDS::Edge(edges(i));
        if (BRep_Tool::Degenerated(edge)) {
            continue;
        }
        double first = 0, last = 0;
        if (BRep_Tool::Curve(edge, first, last).IsNull() || !std::isfinite(first) || !std::isfinite(last)
            || last <= first) {
            continue;
        }
        auto signature = referenceEdge(edge);
        for (auto& p : signature.samples) {
            for (int j = 0; j < 3; ++j) {
                p[j] -= (offset - oldOffset) * unit.Coord(j + 1);
            }
        }
        const BRepAdaptor_Curve curve(edge);
        const int intervals = curve.GetType() == GeomAbs_Line ? 4 : 64;
        SketchProjection projection{signature, {}};
        for (int j = 0; j <= intervals; ++j) {
            const auto p = curve.Value(std::lerp(first, last, static_cast<double>(j) / intervals));
            const gp_Vec delta(origin, p);
            projection.points.push_back({delta.Dot(gp_Vec(frame.XDirection())),
                                         delta.Dot(gp_Vec(frame.YDirection()))});
        }
        result.projections.push_back(std::move(projection));
    }
    return result;
}

TopoDS_Face makeProfile(const Sketch& sketch)
{
    return makeProfile(sketch, sketchContext(sketch));
}

TopoDS_Face makeProfile(const Sketch& sketch, const SketchContext& context)
{
    const auto wires = sketchWires(sketch, context);
    const auto& frame = context.frame;

    const gp_Pln plane(frame);
    struct Loop { TopoDS_Face area; TopoDS_Wire wire; double size; };
    std::vector<Loop> loops;
    for (int i = 1; i <= wires->Length(); ++i) {
        const auto wire = TopoDS::Wire(wires->Value(i));
        if (!BRep_Tool::IsClosed(wire)) {
            throw std::invalid_argument("The profile is open. Connect the last point to the first.");
        }
        BRepBuilderAPI_MakeFace builder(plane, wire, true);
        if (!builder.IsDone()) {
            throw std::invalid_argument("The profile does not enclose a planar face.");
        }
        const auto face = builder.Face();
        BRepCheck_Wire wireCheck(wire);
        TopoDS_Edge first, second;
        GProp_GProps area;
        BRepGProp::SurfaceProperties(face, area);
        if (wireCheck.SelfIntersect(face, first, second) != BRepCheck_NoError
            || !BRepCheck_Analyzer(face, true).IsValid()
            || !(area.Mass() > std::pow(Precision::Confusion(), 2))) {
            throw std::invalid_argument("Use closed contours with no intersections or repeated edges.");
        }
        loops.push_back({face, BRepTools::OuterWire(face), area.Mass()});
    }
    if (loops.empty()) {
        throw std::invalid_argument("Draw a closed profile before finishing the sketch.");
    }
    const auto outer = std::ranges::max_element(loops, {}, &Loop::size);
    BRepBuilderAPI_MakeFace builder(outer->area);
    for (auto loop = loops.begin(); loop != loops.end(); ++loop) {
        if (loop == outer) {
            continue;
        }
        const TopExp_Explorer edge(loop->wire, TopAbs_EDGE);
        const BRepAdaptor_Curve curve(TopoDS::Edge(edge.Current()));
        const auto sample = curve.Value(curve.FirstParameter());
        if (BRepClass_FaceClassifier(outer->area, sample, Precision::Confusion()).State() != TopAbs_IN) {
            throw std::invalid_argument("Inner contours must lie strictly inside the outer contour.");
        }
        for (auto other = loops.begin(); other != loops.end(); ++other) {
            if (other != outer && other != loop
                && BRepClass_FaceClassifier(other->area, sample, Precision::Confusion()).State() != TopAbs_OUT) {
                throw std::invalid_argument("Inner contours must be separate and cannot be nested.");
            }
        }
        builder.Add(TopoDS::Wire(loop->wire.Reversed()));
    }
    const auto face = builder.Face();
    if (BRepCheck_Face(face).IntersectWires() != BRepCheck_NoError || !BRepCheck_Analyzer(face, true).IsValid()) {
        throw std::invalid_argument("Use closed contours with no intersections or repeated edges.");
    }
    return face;
}

TopoDS_Wire makeTrajectory(const Sketch& sketch, const SketchContext& context)
{
    constexpr auto error = "A path sketch needs one connected open chain.";
    const auto wires = sketchWires(sketch, context);
    if (wires->Length() != 1) {
        throw std::invalid_argument(error);
    }
    const auto wire = TopoDS::Wire(wires->Value(1));
    if (BRep_Tool::IsClosed(wire) || !BRepCheck_Analyzer(wire, true).IsValid()) {
        throw std::invalid_argument(error);
    }
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(wire, TopAbs_EDGE, edges);
    int connected = 0;
    for (BRepTools_WireExplorer it(wire); it.More(); it.Next()) {
        ++connected;
    }
    if (connected != edges.Extent()) {
        throw std::invalid_argument(error);
    }
    return wire;
}

TopoDS_Shape extrude(const TopoDS_Face& face, Plane plane, double distance)
{
    return extrude(face, cardinalFrame(plane), distance);
}

TopoDS_Shape extrude(const TopoDS_Face& face, const gp_Ax3& frame, double distance)
{
    if (!std::isfinite(distance) || std::abs(distance) <= Precision::Confusion()) {
        throw std::invalid_argument("Extrusion distance must be nonzero.");
    }
    BRepPrimAPI_MakePrism operation(face, gp_Vec(frame.Direction()) * distance, true);
    return checkedSolid(operation.Shape());
}

TopoDS_Shape revolve(const TopoDS_Face& face, Plane plane, Axis axis, double offset, double degrees)
{
    return revolve(face, cardinalFrame(plane), axis, offset, degrees);
}

TopoDS_Shape revolve(const TopoDS_Face& face, const gp_Ax3& frame, Axis axis, double offset, double degrees)
{
    if (!std::isfinite(offset) || !std::isfinite(degrees) || degrees <= 0 || degrees > 360) {
        throw std::invalid_argument("Enter a finite axis offset and an angle greater than zero and at most 360°.");
    }
    gp_Trsf toLocal;
    toLocal.SetTransformation(frame);
    const auto localFace = BRepBuilderAPI_Transform(face, toLocal, false).Shape();
    Bnd_Box bounds;
    BRepBndLib::AddOptimal(localFace, bounds, false, false);
    const auto lo = bounds.CornerMin();
    const auto hi = bounds.CornerMax();
    const int across = axis == Axis::U ? 2 : 1;
    if (lo.Coord(across) < offset - Precision::Confusion()
        && hi.Coord(across) > offset + Precision::Confusion()) {
        throw std::invalid_argument("The rotation axis crosses the profile. Change the axis offset.");
    }
    const auto origin = point(frame, axis == Axis::U ? Point2{0, offset} : Point2{offset, 0});
    const auto direction = axis == Axis::U ? frame.XDirection() : frame.YDirection();
    BRepPrimAPI_MakeRevol operation(face, gp_Ax1(origin, direction), degrees * std::numbers::pi / 180.0, true);
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
