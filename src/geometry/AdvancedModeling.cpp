#include "geometry/AdvancedModeling.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace graph3d::geometry {
namespace {

gp_Pnt point(Point3 p)
{
    if (!std::ranges::all_of(p, [](double x) { return std::isfinite(x) && std::abs(x) <= 1.e8; })) {
        throw std::invalid_argument("Enter finite construction coordinates.");
    }
    return {p[0], p[1], p[2]};
}

gp_Dir direction(Point3 p)
{
    const auto x = point(p);
    if (x.XYZ().Modulus() <= Precision::Confusion()) {
        throw std::invalid_argument("A construction direction must be nonzero.");
    }
    return gp_Dir(x.XYZ());
}

gp_Pnt local(const gp_Ax3& frame, Point3 p)
{
    const auto xyz = point(p);
    return frame.Location().Translated(gp_Vec(frame.XDirection()) * xyz.X()
        + gp_Vec(frame.YDirection()) * xyz.Y() + gp_Vec(frame.Direction()) * xyz.Z());
}

TopoDS_Shape validSolid(const TopoDS_Shape& shape, const char* error)
{
    if (shape.IsNull() || !BRepCheck_Analyzer(shape, true).IsValid()) {
        throw std::invalid_argument(error);
    }
    TopExp_Explorer solids(shape, TopAbs_SOLID);
    if (!solids.More()) {
        throw std::invalid_argument(error);
    }
    auto result = solids.Current();
    solids.Next();
    if (solids.More()) {
        throw std::invalid_argument(error);
    }
    GProp_GProps properties;
    BRepGProp::VolumeProperties(result, properties);
    if (!std::isfinite(properties.Mass()) || properties.Mass() <= 1.e-12) {
        throw std::invalid_argument(error);
    }
    return result;
}

TopoDS_Wire sectionWire(const TopoDS_Face& face)
{
    TopExp_Explorer wires(face, TopAbs_WIRE);
    if (wires.More()) {
        wires.Next();
    }
    if (wires.More()) {
        throw std::invalid_argument("Sweep and loft need sections with one outer contour.");
    }
    return BRepTools::OuterWire(face);
}

std::vector<TopoDS_Face> resolveFaces(const TopoDS_Shape& body, std::span<const FaceReference> references)
{
    if (references.empty() || references.size() > 64) {
        throw std::invalid_argument("Select between one and 64 planar faces.");
    }
    std::vector<TopoDS_Face> result;
    for (const auto& reference : references) {
        auto face = resolveExactPlanarFace(body, reference);
        if (std::ranges::any_of(result, [&](const auto& other) { return face.IsSame(other); })) {
            throw std::invalid_argument("Select each face once.");
        }
        result.push_back(std::move(face));
    }
    return result;
}

}  // namespace

gp_Ax3 datumFrame(const DatumPlane& plane)
{
    const auto normal = direction(plane.normal);
    const auto along = std::abs(normal.X()) < .9 ? gp_Dir(1, 0, 0) : gp_Dir(0, 1, 0);
    return {point(plane.origin), normal, along};
}

gp_Ax3 rotationFrame(const DatumAxis& axis, const gp_Ax3& profile)
{
    const auto dir = direction(axis.direction);
    const auto origin = point(axis.origin);
    if (std::abs(dir.Dot(profile.Direction())) > 1.e-7
        || std::abs(gp_Vec(profile.Location(), origin).Dot(gp_Vec(profile.Direction()))) > 1.e-6) {
        throw std::invalid_argument("The rotation axis must lie in the sketch plane.");
    }
    return {origin, profile.Direction(), dir};
}

TopoDS_Shape datumOutline(const DatumPlane& plane)
{
    const auto frame = datumFrame(plane);
    BRepBuilderAPI_MakePolygon wire;
    for (auto corner : {Point3{-40, -40, 0}, Point3{40, -40, 0}, Point3{40, 40, 0}, Point3{-40, 40, 0}}) {
        wire.Add(local(frame, corner));
    }
    wire.Close();
    return wire.Wire();
}

TopoDS_Shape datumOutline(const DatumAxis& axis)
{
    const auto origin = point(axis.origin);
    const gp_Vec along(direction(axis.direction));
    return BRepBuilderAPI_MakeEdge(origin.Translated(-60 * along), origin.Translated(60 * along)).Edge();
}

TopoDS_Shape sweepProfile(const TopoDS_Face& profile, const gp_Ax3& frame, std::span<const Point3> path)
{
    if (path.size() < 2 || path.size() > 128 || path.front()[2] != 0
        || std::abs(path[1][0] - path.front()[0]) > 1.e-7 || std::abs(path[1][1] - path.front()[1]) > 1.e-7) {
        throw std::invalid_argument("Start the path in the sketch plane and make its first segment normal to the sketch.");
    }
    BRepBuilderAPI_MakePolygon spine;
    const auto start = local(frame, path.front());
    auto last = start;
    spine.Add(last);
    for (std::size_t i = 1; i < path.size(); ++i) {
        const auto next = local(frame, path[i]);
        if (last.Distance(next) <= Precision::Confusion()
            || (i + 1 == path.size() && next.Distance(start) <= Precision::Confusion())) {
            throw std::invalid_argument("The sweep path must be open with distinct consecutive points.");
        }
        spine.Add(next);
        last = next;
    }
    const char* failure = "Could not sweep the profile. Check the path bends and profile size.";
    BRepOffsetAPI_MakePipeShell builder(spine.Wire());
    builder.SetTransitionMode(BRepBuilderAPI_RightCorner);
    builder.Add(sectionWire(profile), spine.FirstVertex(), false, false);
    builder.Build();
    if (!builder.IsDone() || !builder.MakeSolid()) {
        throw std::invalid_argument(failure);
    }
    return validSolid(builder.Shape(), failure);
}

TopoDS_Shape loftProfiles(std::span<const TopoDS_Face> profiles, bool ruled)
{
    if (profiles.size() < 2 || profiles.size() > 32) {
        throw std::invalid_argument("Select two to 32 ordered loft sections.");
    }
    const char* failure = "Could not loft the sections. Check their order and spacing.";
    BRepOffsetAPI_ThruSections builder(true, ruled);
    builder.SetMutableInput(false);
    builder.CheckCompatibility();
    for (const auto& profile : profiles) {
        builder.AddWire(sectionWire(profile));
    }
    builder.Build();
    if (!builder.IsDone()) {
        throw std::invalid_argument(failure);
    }
    return validSolid(builder.Shape(), failure);
}

TopoDS_Shape shellBody(const TopoDS_Shape& body, std::span<const FaceReference> openings, double thickness)
{
    if (!std::isfinite(thickness) || std::abs(thickness) <= Precision::Confusion()) {
        throw std::invalid_argument("Enter a nonzero shell wall thickness.");
    }
    const auto solid = validSolid(body, "Shell requires one valid solid.");
    TopTools_ListOfShape removed;
    for (const auto& opening : resolveFaces(solid, openings)) {
        removed.Append(opening);
    }
    const char* failure = "Could not create the shell. Check the opening faces and wall thickness.";
    BRepOffsetAPI_MakeThickSolid builder;
    builder.MakeThickSolidByJoin(solid, removed, -thickness, 1.e-5, BRepOffset_Skin, false, false, GeomAbs_Intersection);
    if (!builder.IsDone()) {
        throw std::invalid_argument(failure);
    }
    return validSolid(builder.Shape(), failure);
}

TopoDS_Shape draftBody(const TopoDS_Shape& body, const FaceReference& neutral,
                       std::span<const FaceReference> walls, double degrees)
{
    if (!std::isfinite(degrees) || std::abs(degrees) <= 1.e-6 || std::abs(degrees) >= 89) {
        throw std::invalid_argument("Enter a draft angle between -89 and 89 degrees, excluding zero.");
    }
    const auto solid = validSolid(body, "Draft requires one valid solid.");
    const auto fixed = resolveExactPlanarFace(solid, neutral);
    const auto plane = BRepAdaptor_Surface(fixed).Plane();
    const gp_Dir pull = direction(neutral.outward).Reversed();
    const char* failure = "Could not draft the selected faces. Try a smaller angle.";
    BRepOffsetAPI_DraftAngle builder(solid);
    for (const auto& face : resolveFaces(solid, walls)) {
        if (face.IsSame(fixed) || std::abs(BRepAdaptor_Surface(face).Plane().Axis().Direction().Dot(pull)) > .999999) {
            throw std::invalid_argument("Select walls that are not parallel to the neutral plane.");
        }
        builder.Add(face, pull, degrees * std::numbers::pi / 180.0, plane);
        if (!builder.AddDone()) {
            throw std::invalid_argument(failure);
        }
    }
    builder.Build();
    if (!builder.IsDone()) {
        throw std::invalid_argument(failure);
    }
    return validSolid(builder.Shape(), failure);
}

MassProperties massProperties(const TopoDS_Shape& body, double densityKgM3)
{
    if (body.IsNull() || !std::isfinite(densityKgM3) || densityKgM3 < 0 || densityKgM3 > 1.e9
        || !TopExp_Explorer(body, TopAbs_SOLID).More()) {
        throw std::invalid_argument("Select a solid and enter a valid material density.");
    }
    GProp_GProps g;
    BRepGProp::VolumeProperties(body, g, true);
    const auto volume = g.Mass();
    const auto center = g.CentreOfMass();
    if (!std::isfinite(volume) || volume <= 0) {
        throw std::invalid_argument("The body has no measurable volume.");
    }
    return {volume, {center.X(), center.Y(), center.Z()}, volume * densityKgM3 / 1.e9};
}

double minimumDistance(const TopoDS_Shape& first, const TopoDS_Shape& second)
{
    if (first.IsNull() || second.IsNull()) {
        throw std::invalid_argument("Select two measurable objects.");
    }
    BRepExtrema_DistShapeShape extrema(first, second);
    if (!extrema.IsDone()) {
        throw std::invalid_argument("Could not measure the distance between the objects.");
    }
    return extrema.Value();
}

}  // namespace graph3d::geometry
