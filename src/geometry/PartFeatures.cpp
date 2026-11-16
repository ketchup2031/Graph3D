#include "geometry/PartFeatures.h"

#include "geometry/SketchModeling.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace graph3d::geometry {
namespace {

constexpr double tolerance = 1.e-5;

gp_Pnt point(const Point3& p) { return {p[0], p[1], p[2]}; }
Point3 xyz(const gp_XYZ& p) { return {p.X(), p.Y(), p.Z()}; }

bool finite(const Point3& p)
{
    return std::all_of(p.begin(), p.end(), [](double value) { return std::isfinite(value); });
}

void positive(double value, const char* description)
{
    if (!std::isfinite(value) || value <= tolerance) {
        throw std::invalid_argument(description);
    }
}

TopoDS_Solid singleSolid(const TopoDS_Shape& shape)
{
    TopTools_IndexedMapOfShape solids;
    if (!shape.IsNull()) {
        TopExp::MapShapes(shape, TopAbs_SOLID, solids);
    }
    if (solids.Extent() != 1) {
        throw std::invalid_argument("This operation requires one solid body.");
    }
    return TopoDS::Solid(solids(1));
}

gp_Dir outwardNormal(const TopoDS_Face& face, const gp_Pln& plane)
{
    auto outward = plane.Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) {
        outward.Reverse();
    }
    return outward;
}

bool insideFace(const TopoDS_Face& face, const gp_Pnt& center)
{
    return BRepClass_FaceClassifier(face, center, tolerance).State() == TopAbs_IN;
}

TopoDS_Face findFace(const TopoDS_Solid& solid, const FaceReference& reference)
{
    if (!finite(reference.center) || !finite(reference.outward)
        || std::abs(std::hypot(reference.outward[0], reference.outward[1], reference.outward[2]) - 1) > tolerance) {
        throw std::invalid_argument("Invalid planar face reference.");
    }
    const auto center = point(reference.center);
    TopoDS_Face result;
    const auto normal = gp_Dir(reference.outward[0], reference.outward[1], reference.outward[2]);
    for (TopExp_Explorer it(solid, TopAbs_FACE); it.More(); it.Next()) {
        const auto face = TopoDS::Face(it.Current());
        const BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane) {
            continue;
        }
        const auto plane = surface.Plane();
        if (plane.Distance(center) <= tolerance && outwardNormal(face, plane).Dot(normal) > 1 - 1.e-8
            && insideFace(face, center)) {
            if (!result.IsNull()) {
                throw std::invalid_argument("The original face is ambiguous after rebuilding.");
            }
            result = face;
        }
    }
    if (result.IsNull()) {
        throw std::invalid_argument("The original planar face could not be matched after rebuilding.");
    }
    return result;
}

double distanceToBoundary(const TopoDS_Face& face, const gp_Pnt& center)
{
    const auto vertex = BRepBuilderAPI_MakeVertex(center).Vertex();
    double clearance = RealLast();
    for (TopExp_Explorer it(face, TopAbs_EDGE); it.More(); it.Next()) {
        const BRepExtrema_DistShapeShape distance(vertex, it.Current());
        if (!distance.IsDone() || distance.NbSolution() == 0) {
            throw std::runtime_error("Could not check the hole against the face boundary.");
        }
        clearance = std::min(clearance, distance.Value());
    }
    return clearance;
}

double volume(const TopoDS_Shape& shape)
{
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return properties.Mass();
}

TopoDS_Shape checkedResult(const TopoDS_Shape& source, const TopoDS_Shape& result)
{
    const auto solid = singleSolid(result);
    const double before = volume(source);
    const double after = volume(solid);
    if (!BRepCheck_Analyzer(solid, true).IsValid()
        || !(after > 0 && std::abs(before - after) > tolerance * tolerance * tolerance)) {
        throw std::runtime_error("The operation did not produce a changed, valid solid.");
    }
    return solid;
}

bool sameEdge(const EdgeReference& a, const EdgeReference& b)
{
    if (a.curveType != b.curveType) {
        return false;
    }
    bool forward = true;
    bool reverse = true;
    for (std::size_t i = 0; i < a.samples.size(); ++i) {
        forward = forward && point(a.samples[i]).Distance(point(b.samples[i])) <= tolerance;
        reverse = reverse && point(a.samples[i]).Distance(point(b.samples[a.samples.size() - 1 - i])) <= tolerance;
    }
    return forward || reverse;
}

TopoDS_Edge findEdge(const TopoDS_Solid& solid, const EdgeReference& reference)
{
    for (const auto& p : reference.samples) {
        if (!finite(p)) {
            throw std::invalid_argument("Invalid edge reference.");
        }
    }
    TopTools_IndexedDataMapOfShapeListOfShape neighbors;
    TopExp::MapShapesAndUniqueAncestors(solid, TopAbs_EDGE, TopAbs_FACE, neighbors);
    TopoDS_Edge result;
    for (int i = 1; i <= neighbors.Extent(); ++i) {
        const auto edge = TopoDS::Edge(neighbors.FindKey(i));
        double first = 0;
        double last = 0;
        if (neighbors.FindFromIndex(i).Size() != 2 || BRep_Tool::Degenerated(edge)
            || BRep_Tool::Curve(edge, first, last).IsNull()) {
            continue;
        }
        if (sameEdge(referenceEdge(edge), reference)) {
            if (!result.IsNull()) {
                throw std::invalid_argument("The original edge is ambiguous after rebuilding.");
            }
            result = edge;
        }
    }
    if (result.IsNull()) {
        throw std::invalid_argument("The original two-face edge could not be matched after rebuilding.");
    }
    return result;
}

template<class Builder>
TopoDS_Shape finishTreatment(const TopoDS_Solid& body, const TopoDS_Edge& edge, double size)
{
    Builder operation(body);
    operation.Add(size, edge);
    if (operation.NbContours() != 1 || operation.NbEdges(1) != 1) {
        throw std::invalid_argument("Select one sharp edge. This operation cannot follow a chain of tangent edges.");
    }
    operation.Build();
    if (!operation.IsDone()) {
        throw std::runtime_error("Open CASCADE could not treat this edge. Try a smaller size.");
    }
    return checkedResult(body, operation.Shape());
}

}  // namespace

FaceReference locateHole(const TopoDS_Face& face, Point3 rayOrigin, Point3 rayDirection)
{
    const BRepAdaptor_Surface surface(face);
    if (surface.GetType() != GeomAbs_Plane || !finite(rayOrigin) || !finite(rayDirection)) {
        throw std::invalid_argument("Click a planar face to place the hole.");
    }
    const auto plane = surface.Plane();
    const auto outward = outwardNormal(face, plane);
    const gp_Vec direction(rayDirection[0], rayDirection[1], rayDirection[2]);
    const auto origin = point(rayOrigin);
    const auto alignment = outward.XYZ().Dot(direction.XYZ());
    if (std::abs(alignment) < 1.e-8) {
        throw std::invalid_argument("Rotate the body to face the hole location.");
    }
    const auto t = outward.XYZ().Dot(gp_Vec(origin, plane.Location()).XYZ()) / alignment;
    const auto center = origin.Translated(direction * t);
    if (!insideFace(face, center)) {
        throw std::invalid_argument("Click inside the face, away from its edge.");
    }
    return {xyz(center.XYZ()), xyz(outward.XYZ())};
}

EdgeReference referenceEdge(const TopoDS_Edge& edge)
{
    double first = 0;
    double last = 0;
    if (BRep_Tool::Degenerated(edge) || BRep_Tool::Curve(edge, first, last).IsNull()
        || !std::isfinite(first) || !std::isfinite(last) || first >= last) {
        throw std::invalid_argument("Select a finite, non-degenerate edge.");
    }
    const BRepAdaptor_Curve curve(edge);
    EdgeReference result{{}, static_cast<int>(curve.GetType())};
    for (std::size_t i = 0; i < result.samples.size(); ++i) {
        const double t = static_cast<double>(i) / (result.samples.size() - 1);
        result.samples[i] = xyz(curve.Value(std::lerp(first, last, t)).XYZ());
    }
    return result;
}

TopoDS_Shape drillHole(const TopoDS_Shape& body, const FaceReference& reference, HoleSize size)
{
    positive(size.diameter, "The hole diameter must be positive.");
    if (!size.through) {
        positive(size.depth, "The blind hole depth must be positive.");
    }
    const auto solid = singleSolid(body);
    const auto face = findFace(solid, reference);
    const auto center = point(reference.center);
    const double radius = size.diameter / 2;
    if (radius + tolerance >= distanceToBoundary(face, center)) {
        throw std::invalid_argument("The hole must fit inside the selected face.");
    }
    const gp_Dir inward(-reference.outward[0], -reference.outward[1], -reference.outward[2]);
    const auto direction = gp_Vec(inward);
    const double margin = std::max(tolerance * 4, radius * 1.e-6);
    double depth = size.depth;
    if (size.through) {
        Bnd_Box bounds;
        BRepBndLib::AddOptimal(solid, bounds, false, false);
        const auto low = bounds.CornerMin();
        const auto high = bounds.CornerMax();
        depth = 0;
        for (const auto x : {low.X(), high.X()}) {
            for (const auto y : {low.Y(), high.Y()}) {
                for (const auto z : {low.Z(), high.Z()}) {
                    depth = std::max(depth, gp_Vec(center, gp_Pnt(x, y, z)).Dot(direction));
                }
            }
        }
        depth += margin;
    } else {
        const auto bottom = center.Translated(direction * (depth + margin));
        if (BRepClass3d_SolidClassifier(solid, bottom, tolerance).State() != TopAbs_IN) {
            throw std::invalid_argument("The blind hole breaks through or leaves the solid. Reduce the depth or use Through.");
        }
    }
    const auto start = center.Translated(-direction * margin);
    const auto cutter = BRepPrimAPI_MakeCylinder(gp_Ax2(start, inward), radius, depth + margin).Shape();
    const auto result = checkedResult(solid, booleanResult(solid, cutter, BooleanOperation::Difference));
    const double removed = volume(solid) - volume(result);
    if (!(removed > 0)) {
        throw std::runtime_error("The hole did not remove material.");
    }
    if (!size.through) {
        const double expected = std::numbers::pi * radius * radius * depth;
        if (std::abs(removed - expected) > std::max(1.e-6, expected * 1.e-6)) {
            throw std::invalid_argument("The blind hole intersects another surface or void.");
        }
    }
    return result;
}

TopoDS_Shape treatEdge(const TopoDS_Shape& body, const EdgeReference& reference, EdgeTreatmentKind kind, double size)
{
    positive(size, "The chamfer distance or fillet radius must be positive.");
    const auto solid = singleSolid(body);
    const auto edge = findEdge(solid, reference);
    switch (kind) {
    case EdgeTreatmentKind::Chamfer: return finishTreatment<BRepFilletAPI_MakeChamfer>(solid, edge, size);
    case EdgeTreatmentKind::Fillet: return finishTreatment<BRepFilletAPI_MakeFillet>(solid, edge, size);
    }
    throw std::invalid_argument("Unknown edge treatment.");
}

}  // namespace graph3d::geometry
