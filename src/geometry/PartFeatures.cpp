#include "geometry/PartFeatures.h"

#include "geometry/SketchModeling.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
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
#include <BRepTools.hxx>
#include <optional>
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
TopoDS_Shape finishTreatment(const TopoDS_Solid& body, const std::vector<TopoDS_Edge>& edges,
                            double size, bool follow, std::optional<double> end)
{
    Builder operation(body);
    for (const auto& edge : edges) {
        if (operation.Contour(edge) == 0) {
            if constexpr (std::is_same_v<Builder, BRepFilletAPI_MakeFillet>) {
                operation.Add(size, edge);
                if (end) {
                    const auto index = operation.Contour(edge);
                    if (!index) { throw std::invalid_argument("Cannot start a fillet on this edge."); }
                    double total = 0;
                    std::vector<double> lengths;
                    for (int j = 1; j <= operation.NbEdges(index); ++j) {
                        GProp_GProps props;
                        BRepGProp::LinearProperties(operation.Edge(index, j), props);
                        lengths.push_back(props.Mass());
                        total += props.Mass();
                    }
                    if (total <= tolerance) { throw std::invalid_argument("The tangent chain is too short."); }
                    if (operation.Closed(index) && size != *end) {
                        throw std::invalid_argument("A closed tangent chain requires equal end radii.");
                    }
                    double position = 0;
                    for (int j = 1; j <= operation.NbEdges(index); ++j) {
                        const auto start = std::lerp(size, *end, position / total);
                        position += lengths[j - 1];
                        operation.SetRadius(start, std::lerp(size, *end, position / total), index, j);
                    }
                }
            } else {
                operation.Add(size, edge);
            }
        }
    }
    for (const auto& edge : edges) {
        if (operation.Contour(edge) == 0) {
            throw std::invalid_argument("Every selected edge must support the edge treatment.");
        }
    }
    for (int i = 1; !follow && i <= operation.NbContours(); ++i) {
        for (int j = 1; j <= operation.NbEdges(i); ++j) {
            if (std::ranges::none_of(edges, [&](const auto& edge) { return edge.IsSame(operation.Edge(i, j)); })) {
                throw std::invalid_argument("Select every edge in a tangent chain.");
            }
        }
    }
    operation.Build();
    if (!operation.IsDone()) {
        throw std::runtime_error("Open CASCADE could not treat this edge. Try a smaller size.");
    }
    return checkedResult(body, operation.Shape());
}

}  // namespace

FaceReference locateHole(const TopoDS_Face& face, Point3 rayOrigin, Point3 rayDirection, bool sketch)
{
    const BRepAdaptor_Surface surface(face);
    if (surface.GetType() != GeomAbs_Plane || !finite(rayOrigin) || !finite(rayDirection)) {
        throw std::invalid_argument(sketch ? "Click a planar face for the sketch." : "Click a planar face to place the hole.");
    }
    const auto plane = surface.Plane();
    const auto outward = outwardNormal(face, plane);
    const gp_Vec direction(rayDirection[0], rayDirection[1], rayDirection[2]);
    const auto origin = point(rayOrigin);
    const auto alignment = outward.XYZ().Dot(direction.XYZ());
    if (std::abs(alignment) < 1.e-8) {
        throw std::invalid_argument(sketch ? "Rotate the body to face the sketch plane." : "Rotate the body to face the hole location.");
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

std::vector<FaceReference> planarFaces(const TopoDS_Shape& body)
{
    const auto solid = singleSolid(body);
    std::vector<FaceReference> result;
    for (TopExp_Explorer it(solid, TopAbs_FACE); it.More(); it.Next()) {
        const auto face = TopoDS::Face(it.Current());
        const BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane) {
            continue;
        }
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        std::optional<gp_Pnt> sample;
        for (double u : {.5, .2, .8, .1, .9, .35, .65}) {
            for (double v : {.5, .2, .8, .1, .9, .35, .65}) {
                const auto p = surface.Value(std::lerp(u0, u1, u), std::lerp(v0, v1, v));
                if (insideFace(face, p)) {
                    sample = p;
                    break;
                }
            }
            if (sample) {
                break;
            }
        }
        if (sample) {
            result.push_back({xyz(sample->XYZ()), xyz(outwardNormal(face, surface.Plane()).XYZ())});
        }
    }
    return result;
}

std::vector<EdgeReference> twoFaceEdges(const TopoDS_Shape& body)
{
    TopTools_IndexedDataMapOfShapeListOfShape neighbors;
    TopExp::MapShapesAndUniqueAncestors(singleSolid(body), TopAbs_EDGE, TopAbs_FACE, neighbors);
    std::vector<EdgeReference> result;
    for (int i = 1; i <= neighbors.Extent(); ++i) {
        const auto edge = TopoDS::Edge(neighbors.FindKey(i));
        double first = 0, last = 0;
        if (neighbors.FindFromIndex(i).Size() == 2 && !BRep_Tool::Degenerated(edge)
            && !BRep_Tool::Curve(edge, first, last).IsNull()) {
            result.push_back(referenceEdge(edge));
        }
    }
    return result;
}

TopoDS_Face resolveExactPlanarFace(const TopoDS_Shape& body, const FaceReference& reference)
{
    return findFace(singleSolid(body), reference);
}

TopoDS_Face resolveSketchFace(const TopoDS_Shape& body, const FaceReference& reference)
{
    const auto solid = singleSolid(body);
    if (!finite(reference.center) || !finite(reference.outward)
        || std::abs(std::hypot(reference.outward[0], reference.outward[1], reference.outward[2]) - 1) > tolerance) {
        throw std::invalid_argument("Invalid planar face reference.");
    }
    const auto anchor = point(reference.center);
    const auto outward = gp_Dir(reference.outward[0], reference.outward[1], reference.outward[2]);
    TopoDS_Face exact, shifted;
    int exactCount = 0;
    int shiftedCount = 0;
    for (TopExp_Explorer it(solid, TopAbs_FACE); it.More(); it.Next()) {
        const auto face = TopoDS::Face(it.Current());
        const BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane) {
            continue;
        }
        const auto plane = surface.Plane();
        if (outwardNormal(face, plane).Dot(outward) < 1 - 1.e-8) {
            continue;
        }
        const double delta = gp_Vec(plane.Location(), anchor).Dot(gp_Vec(outward));
        if (!insideFace(face, anchor.Translated(gp_Vec(outward) * -delta))) {
            continue;
        }
        shifted = face;
        ++shiftedCount;
        if (std::abs(delta) <= tolerance) {
            exact = face;
            ++exactCount;
        }
    }
    if (exactCount == 1) {
        return exact;
    }
    if (exactCount == 0 && shiftedCount == 1) {
        return shifted;
    }
    if (shiftedCount > 1) {
        throw std::invalid_argument("The sketch support face is ambiguous after rebuilding.");
    }
    throw std::invalid_argument("The sketch support face could not be matched after rebuilding.");
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
    double head = radius;
    double recess = 0;
    if (size.style != HoleStyle::Straight) {
        positive(size.headDiameter, "The head diameter must be positive.");
        head = size.headDiameter / 2;
        if (head <= radius + tolerance) { throw std::invalid_argument("The head must be wider than the hole."); }
        if (size.style == HoleStyle::Counterbore) {
            recess = size.seat;
        } else if (size.style == HoleStyle::Countersink) {
            if (!std::isfinite(size.seat) || size.seat < 30 || size.seat > 150) {
                throw std::invalid_argument("Use a countersink angle between 30 and 150 degrees.");
            }
            recess = (head - radius) / std::tan(size.seat * std::numbers::pi / 360);
        } else {
            throw std::invalid_argument("Unknown hole style.");
        }
        positive(recess, "The recess depth must be positive.");
    }
    if (head + tolerance >= distanceToBoundary(face, center)) {
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
    if (recess >= depth - tolerance
        || (recess && BRepClass3d_SolidClassifier(solid, center.Translated(direction * (recess + margin)), tolerance).State() != TopAbs_IN)) {
        throw std::invalid_argument("The head recess must end inside the hole.");
    }
    if (size.thread) {
        const auto& thread = *size.thread;
        positive(thread.pitch, "The thread pitch must be positive.");
        positive(thread.length, "The thread length must be positive.");
        positive(thread.diameter, "The thread diameter must be positive.");
        if (thread.diameter < size.diameter || thread.pitch >= thread.diameter || thread.length > depth - recess + tolerance
            || BRepClass3d_SolidClassifier(solid,
                center.Translated(direction * (recess + thread.length - margin)), tolerance).State() != TopAbs_IN) {
            throw std::invalid_argument("The thread must fit inside the available hole depth.");
        }
    }
    auto result = checkedResult(solid, booleanResult(solid, cutter, BooleanOperation::Difference));
    if (recess) {
        TopoDS_Shape opening;
        if (size.style == HoleStyle::Counterbore) {
            opening = BRepPrimAPI_MakeCylinder(gp_Ax2(start, inward), head, recess + margin).Shape();
        } else {
            const auto slope = (head - radius) / recess;
            opening = BRepPrimAPI_MakeCone(gp_Ax2(start, inward), head + margin * slope, radius, recess + margin).Shape();
        }
        result = checkedResult(result, booleanResult(result, opening, BooleanOperation::Difference));
    }
    const double removed = volume(solid) - volume(result);
    if (!(removed > 0)) {
        throw std::runtime_error("The hole did not remove material.");
    }
    if (!size.through) {
        const double extra = size.style == HoleStyle::Counterbore ? (head * head - radius * radius) * recess
            : size.style == HoleStyle::Countersink ? (head * head + head * radius - 2 * radius * radius) * recess / 3 : 0;
        const double expected = std::numbers::pi * (radius * radius * depth + extra);
        if (std::abs(removed - expected) > std::max(1.e-6, expected * 1.e-6)) {
            throw std::invalid_argument("The blind hole intersects another surface or void.");
        }
    }
    return result;
}

TopoDS_Shape treatEdge(const TopoDS_Shape& body, const EdgeReference& reference, EdgeTreatmentKind kind, double size)
{
    return treatEdges(body, std::span<const EdgeReference>(&reference, 1), kind, size);
}

TopoDS_Shape treatEdges(const TopoDS_Shape& body, std::span<const EdgeReference> references,
                       EdgeTreatmentKind kind, double size, bool follow, std::optional<double> end)
{
    positive(size, "Enter a positive chamfer distance or fillet radius.");
    if (end) {
        positive(*end, "Enter a positive end radius.");
        if (kind != EdgeTreatmentKind::Fillet) { throw std::invalid_argument("Only fillets support a variable radius."); }
    }
    if (references.empty() || references.size() > 64) {
        throw std::invalid_argument("Select between one and 64 edges.");
    }
    const auto solid = singleSolid(body);
    std::vector<TopoDS_Edge> edges;
    for (const auto& reference : references) {
        const auto edge = findEdge(solid, reference);
        if (std::ranges::any_of(edges, [&](const auto& selected) { return selected.IsSame(edge); })) {
            throw std::invalid_argument("Select each edge once.");
        }
        edges.push_back(edge);
    }
    switch (kind) {
    case EdgeTreatmentKind::Chamfer: return finishTreatment<BRepFilletAPI_MakeChamfer>(solid, edges, size, follow, end);
    case EdgeTreatmentKind::Fillet: return finishTreatment<BRepFilletAPI_MakeFillet>(solid, edges, size, follow, end);
    }
    throw std::invalid_argument("Unknown edge treatment.");
}

}  // namespace graph3d::geometry
