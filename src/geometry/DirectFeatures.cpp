#include "geometry/PartFeatures.h"
#include "geometry/SketchModeling.h"

#include <BRepAdaptor_CompCurve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GCPnts_UniformAbscissa.hxx>
#include <GProp_GProps.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace graph3d::geometry {
namespace {
constexpr double eps = 1.e-6;
double volume(const TopoDS_Shape& shape)
{
    GProp_GProps g;
    BRepGProp::VolumeProperties(shape, g);
    return g.Mass();
}
TopoDS_Shape verified(const TopoDS_Shape& result, const TopoDS_Shape& base, const char* message)
{
    TopTools_IndexedMapOfShape solids;
    TopExp::MapShapes(result, TopAbs_SOLID, solids);
    if (solids.Extent() != 1 || !BRepCheck_Analyzer(solids(1)).IsValid() || volume(solids(1)) <= eps
        || std::abs(volume(solids(1)) - volume(base)) <= eps) {
        throw std::invalid_argument(message);
    }
    return solids(1);
}
template<class Operation> TopoDS_Shape boolean(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    Operation op;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(a);
    tools.Append(b);
    op.SetArguments(arguments);
    op.SetTools(tools);
    op.SetNonDestructive(true);
    op.Build();
    if (!op.IsDone()) { throw std::invalid_argument("The edit could not be applied to this solid."); }
    return op.Shape();
}
}

BlendReference referenceBlend(const TopoDS_Face& face)
{
    const auto type = BRepAdaptor_Surface(face).GetType();
    if (type != GeomAbs_Cylinder && type != GeomAbs_Torus && type != GeomAbs_Sphere
        && type != GeomAbs_BSplineSurface && type != GeomAbs_BezierSurface) {
        throw std::invalid_argument("Select a curved blend face.");
    }
    GProp_GProps g;
    BRepGProp::SurfaceProperties(face, g);
    const auto p = g.CentreOfMass();
    return {{p.X(), p.Y(), p.Z()}, g.Mass(), static_cast<int>(type)};
}

std::vector<BlendReference> blendFaces(const TopoDS_Shape& body)
{
    std::vector<BlendReference> faces;
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(body, TopAbs_FACE, map);
    for (int i = 1; i <= map.Extent(); ++i) {
        try { faces.push_back(referenceBlend(TopoDS::Face(map(i)))); }
        catch (const std::invalid_argument&) {}
    }
    return faces;
}

TopoDS_Shape removeBlend(const TopoDS_Shape& body, const BlendReference& reference)
{
    const auto working = BRepBuilderAPI_Copy(body).Shape();
    TopoDS_Face selected;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(working, TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
        const auto face = TopoDS::Face(faces(i));
        if (static_cast<int>(BRepAdaptor_Surface(face).GetType()) != reference.surface) { continue; }
        const auto candidate = referenceBlend(face);
        if (std::abs(candidate.area - reference.area) < eps
            && std::hypot(candidate.center[0] - reference.center[0], candidate.center[1] - reference.center[1],
                          candidate.center[2] - reference.center[2]) < eps) {
            if (!selected.IsNull()) { throw std::invalid_argument("The selected blend is ambiguous."); }
            selected = face;
        }
    }
    if (selected.IsNull()) { throw std::invalid_argument("The selected blend is missing."); }
    BRepAlgoAPI_Defeaturing algorithm;
    algorithm.SetShape(working);
    algorithm.AddFaceToRemove(selected);
    algorithm.Build();
    if (!algorithm.IsDone() || algorithm.HasWarnings()) {
        throw std::invalid_argument("The adjacent surfaces could not be restored for this blend.");
    }
    return verified(algorithm.Shape(), body, "Blend removal did not produce one changed, valid solid.");
}

TopoDS_Shape movePlanarFace(const TopoDS_Shape& body, const FaceReference& reference, double distance)
{
    if (!std::isfinite(distance) || std::abs(distance) < eps || std::abs(distance) > 1.e6) {
        throw std::invalid_argument("Enter a finite, nonzero face displacement.");
    }
    const auto working = BRepBuilderAPI_Copy(body).Shape();
    const auto face = resolveExactPlanarFace(working, reference);
    const auto& n = reference.outward;
    const gp_Vec normal(n[0], n[1], n[2]);
    TopTools_IndexedDataMapOfShapeListOfShape neighbors;
    TopExp::MapShapesAndUniqueAncestors(working, TopAbs_EDGE, TopAbs_FACE, neighbors);
    for (TopExp_Explorer edges(face, TopAbs_EDGE); edges.More(); edges.Next()) {
        for (const auto& other : neighbors.FindFromKey(edges.Current())) {
            if (other.IsSame(face)) { continue; }
            const BRepAdaptor_Surface surface(TopoDS::Face(other));
            const bool extendable = (surface.GetType() == GeomAbs_Plane
                    && std::abs(normal.Dot(gp_Vec(surface.Plane().Axis().Direction()))) < eps)
                || (surface.GetType() == GeomAbs_Cylinder
                    && std::abs(normal.Dot(gp_Vec(surface.Cylinder().Axis().Direction()))) > 1 - eps);
            if (!extendable) {
                throw std::invalid_argument("The face must meet straight neighboring walls in the offset direction.");
            }
        }
    }
    // Overlap the original volume for a fuse. A prism that only touches the imported
    // STEP face can leave unmatched tolerances between coincident edges.
    const auto overlap = distance > 0 ? std::min(.01, distance * .1) : 0.;
    auto clean = BRepBuilderAPI_Copy(face).Shape().Oriented(TopAbs_FORWARD);
    if (overlap) {
        gp_Trsf inset;
        inset.SetTranslation(-normal * overlap);
        clean = BRepBuilderAPI_Transform(clean, inset, true).Shape();
    }
    auto prism = BRepPrimAPI_MakePrism(clean, normal * (distance + overlap), true).Shape();
    if (volume(prism) < 0) { prism.Reverse(); }
    TopTools_IndexedMapOfShape originals;
    TopExp::MapShapes(working, TopAbs_SOLID, originals);
    if (originals.Extent() != 1) { throw std::invalid_argument("Face displacement requires one solid."); }
    const auto& original = originals(1);
    const auto changed = distance > 0 ? boolean<BRepAlgoAPI_Fuse>(original, prism) : boolean<BRepAlgoAPI_Cut>(original, prism);
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    if (std::abs(volume(changed) - volume(body) - props.Mass() * distance)
        > std::max(eps, std::abs(props.Mass() * distance) * 1.e-6)) {
        throw std::invalid_argument("The displaced face would intersect another feature or leave the body.");
    }
    ShapeUpgrade_UnifySameDomain merge(changed, true, true, true);
    merge.Build();
    const auto unified = merge.Shape();
    if (BRepCheck_Analyzer(unified).IsValid()) {
        return verified(unified, body, "Face displacement did not produce one changed, valid solid.");
    }
    return verified(changed, body, "Face displacement did not produce one changed, valid solid.");
}

std::vector<Point3> pathArrayOffsets(const TopoDS_Wire& path, int count, bool reverse)
{
    if (count < 2 || count > 100) { throw std::invalid_argument("Use 2 to 100 pattern instances."); }
    BRepAdaptor_CompCurve curve(path, true);
    const GCPnts_UniformAbscissa samples(curve, count + (path.Closed() ? 1 : 0));
    if (!samples.IsDone() || samples.NbPoints() < count) {
        throw std::invalid_argument("The pattern path could not be divided by length.");
    }
    const auto pointAt = [&](int i) {
        return curve.Value(samples.Parameter(reverse ? samples.NbPoints() + 1 - i : i));
    };
    const auto origin = pointAt(1);
    std::vector<Point3> result;
    for (int i = 2; i <= count; ++i) {
        const gp_Vec displacement(origin, pointAt(i));
        if (displacement.Magnitude() < eps) { throw std::invalid_argument("The path has duplicate pattern positions."); }
        result.push_back({displacement.X(), displacement.Y(), displacement.Z()});
    }
    return result;
}

TopoDS_Shape patternFeature(const TopoDS_Shape& base, const TopoDS_Shape& seed, std::span<const Point3> offsets)
{
    if (offsets.empty() || offsets.size() >= 100) { throw std::invalid_argument("Use 2 to 100 pattern instances."); }
    const auto removed = boolean<BRepAlgoAPI_Cut>(base, seed);
    const auto added = boolean<BRepAlgoAPI_Cut>(seed, base);
    const bool subtract = volume(removed) > eps;
    const bool add = volume(added) > eps;
    if (!subtract && !add) { throw std::invalid_argument("The seed operation did not change the base solid."); }
    auto result = seed;
    for (const auto& offset : offsets) {
        if (!std::isfinite(std::hypot(offset[0], offset[1], offset[2]))
            || std::hypot(offset[0], offset[1], offset[2]) <= eps) {
            throw std::invalid_argument("The pattern instance offset must be finite and nonzero.");
        }
        gp_Trsf transform;
        transform.SetTranslation(gp_Vec(offset[0], offset[1], offset[2]));
        const auto before = result;
        if (subtract) { result = boolean<BRepAlgoAPI_Cut>(result, BRepBuilderAPI_Transform(removed, transform).Shape()); }
        if (add) { result = boolean<BRepAlgoAPI_Fuse>(result, BRepBuilderAPI_Transform(added, transform).Shape()); }
        result = verified(result, before, "A pattern instance did not produce one changed, valid solid.");
    }
    return result;
}
}  // namespace graph3d::geometry
