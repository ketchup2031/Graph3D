#include "geometry/SurfaceModeling.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_RectangularTrimmedSurface.hxx>
#include <GeomConvert.hxx>
#include <GeomLib.hxx>
#include <GeomPlate_BuildPlateSurface.hxx>
#include <GeomPlate_CurveConstraint.hxx>
#include <GeomPlate_MakeApprox.hxx>
#include <GeomPlate_PointConstraint.hxx>
#include <GeomPlate_Surface.hxx>
#include <GProp_GProps.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <TColGeom2d_HArray1OfCurve.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Solid.hxx>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace graph3d::geometry {
namespace {

constexpr double tolerance = 1.e-6;

TopoDS_Wire wire(const TopoDS_Shape& shape)
{
    if (shape.ShapeType() == TopAbs_WIRE) { return TopoDS::Wire(shape); }
    if (shape.ShapeType() == TopAbs_EDGE) {
        return BRepBuilderAPI_MakeWire(TopoDS::Edge(shape)).Wire();
    }
    if (shape.ShapeType() == TopAbs_FACE) { return BRepTools::OuterWire(TopoDS::Face(shape)); }
    TopExp_Explorer wires(shape, TopAbs_WIRE);
    if (wires.More()) {
        const auto result = TopoDS::Wire(wires.Current());
        wires.Next();
        if (!wires.More()) { return result; }
    }
    throw std::invalid_argument("Select exactly one continuous curve or section.");
}

TopoDS_Shape checked(TopoDS_Shape shape, const char* message)
{
    if (shape.IsNull() || !BRepCheck_Analyzer(shape).IsValid()) { throw std::invalid_argument(message); }
    return shape;
}

TopoDS_Shape sewn(std::span<const TopoDS_Shape> shapes, double gap, bool solid)
{
    if (!std::isfinite(gap) || gap < tolerance || gap > 1) {
        throw std::invalid_argument("Sewing tolerance must be from 0.000001 to 1 mm.");
    }
    BRepBuilderAPI_Sewing sewing(gap);
    for (const auto& shape : shapes) {
        if (!TopExp_Explorer(shape, TopAbs_FACE).More()) {
            throw std::invalid_argument("Select faces or shells to sew.");
        }
        sewing.Add(shape);
    }
    sewing.Perform();
    if (sewing.NbMultipleEdges() != 0) {
        throw std::invalid_argument("More than two faces meet at a sewn edge.");
    }
    if (!solid) { return checked(sewing.SewedShape(), "Could not sew these surfaces."); }
    if (sewing.NbFreeEdges() != 0) {
        throw std::invalid_argument("Close all open edges before creating a solid.");
    }
    const auto shape = sewing.SewedShape();
    if (shape.IsNull() || shape.ShapeType() != TopAbs_SHELL) {
        throw std::invalid_argument("Select surfaces forming one connected closed shell.");
    }
    auto body = BRepBuilderAPI_MakeSolid(TopoDS::Shell(shape)).Solid();
    if (!BRepLib::OrientClosedSolid(body)) {
        throw std::invalid_argument("Could not orient the closed shell.");
    }
    return checked(body, "The sewn shell cannot form a valid solid.");
}

TopoDS_Shape openEdges(const TopoDS_Shape& shape, bool loops, int index)
{
    if (!TopExp_Explorer(shape, TopAbs_FACE).More()) { throw std::invalid_argument("Select a surface with open edges."); }
    ShapeAnalysis_FreeBounds boundaries(shape, tolerance, false, true);
    const auto closed = boundaries.GetClosedWires();
    if (loops) {
        for (TopExp_Explorer wires(closed, TopAbs_WIRE); wires.More(); wires.Next()) {
            if (--index == 0) { return wires.Current(); }
        }
        throw std::invalid_argument("The requested closed boundary does not exist.");
    }
    BRep_Builder builder;
    TopoDS_Compound result;
    builder.MakeCompound(result);
    for (const auto& group : {closed, boundaries.GetOpenWires()}) {
        for (TopExp_Explorer wires(group, TopAbs_WIRE); wires.More(); wires.Next()) { builder.Add(result, wires.Current()); }
    }
    if (result.NbChildren() == 0) { throw std::invalid_argument("The selected surface has no open edges."); }
    return result;
}

TopoDS_Shape fill(std::span<const TopoDS_Shape> curves, int boundaryCount)
{
    TopTools_ListOfShape boundary;
    std::vector<gp_Pnt> guides;
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const auto path = wire(curves[i]);
        for (TopExp_Explorer edges(path, TopAbs_EDGE); edges.More(); edges.Next()) {
            const auto edge = TopoDS::Edge(edges.Current());
            if (i < static_cast<std::size_t>(boundaryCount)) {
                boundary.Append(edge);
                continue;
            }
            // Only the outer edges define the trim. Interior guides constrain sampled positions.
            const BRepAdaptor_Curve curve(edge);
            for (int sample = 1; sample < 12; ++sample) {
                const auto point = curve.Value(std::lerp(curve.FirstParameter(), curve.LastParameter(), sample / 12.));
                if (std::ranges::none_of(guides, [&](const auto& prior) { return prior.Distance(point) <= tolerance; })) {
                    guides.push_back(point);
                }
            }
            if (guides.size() > 512) { throw std::invalid_argument("The surface network has too many guide constraints."); }
        }
    }
    BRepBuilderAPI_MakeWire contour;
    contour.Add(boundary);
    if (!contour.IsDone() || !BRep_Tool::IsClosed(contour.Wire()) || boundary.Size() > 64) {
        throw std::invalid_argument("The surface network needs one closed boundary.");
    }
    GeomPlate_BuildPlateSurface plate;
    BRepBuilderAPI_MakeFace planarSeed(contour.Wire(), true);
    if (planarSeed.IsDone()) {
        if (guides.empty()) { return checked(planarSeed.Face(), "The planar patch is invalid."); }
        plate.LoadInitSurface(BRep_Tool::Surface(planarSeed.Face()));
    }

    // Keep the connected boundary and its vertices. Add a p-curve to each copied
    // edge of the original trim after fitting the support surface.
    constexpr double fitTolerance = 1.e-4;
    const auto rim = TopoDS::Wire(BRepBuilderAPI_Copy(contour.Wire(), false).Shape());
    std::vector<TopoDS_Edge> edges;
    for (BRepTools_WireExplorer edge(rim); edge.More(); edge.Next()) {
        edges.push_back(edge.Current());
        const Handle(BRepAdaptor_Curve) curve = new BRepAdaptor_Curve(edges.back());
        plate.Add(new GeomPlate_CurveConstraint(curve, 0, 18, fitTolerance));
    }
    if (edges.size() != static_cast<std::size_t>(boundary.Size())) {
        throw std::invalid_argument("The surface network needs one closed boundary.");
    }
    plate.SetNbBounds(static_cast<int>(edges.size()));
    for (const auto& point : guides) { plate.Add(new GeomPlate_PointConstraint(point, 0, fitTolerance)); }
    plate.Perform();
    if (!plate.IsDone() || plate.Surface().IsNull()) {
        throw std::invalid_argument("The boundary or curve network could not be filled.");
    }
    const auto fitError = std::max(fitTolerance, plate.G0Error());
    const GeomPlate_MakeApprox spline(plate.Surface(), fitTolerance, 9, 8, fitError);
    const auto surface = spline.Surface();
    const auto curvesOnPlate = plate.Curves2d();
    if (surface.IsNull() || curvesOnPlate.IsNull() || curvesOnPlate->Length() < static_cast<int>(edges.size())) {
        throw std::invalid_argument("Filling did not create a valid surface.");
    }
    BRep_Builder builder;
    for (int i = 0; i < static_cast<int>(edges.size()); ++i) {
        const auto curve = curvesOnPlate->Value(i + 1);
        if (curve.IsNull()) { throw std::invalid_argument("Filling did not create a valid surface."); }
        builder.UpdateEdge(edges[i], curve, surface, TopLoc_Location(), fitError + spline.ApproxError());
    }
    BRepBuilderAPI_MakeFace face(surface, rim, true);
    if (!face.IsDone()) { throw std::invalid_argument("Filling did not create a valid surface."); }
    BRepLib::SameParameter(face.Face(), fitTolerance, true);
    return checked(face.Face(), "Filling did not create a valid surface.");
}

}  // namespace

TopoDS_Shape makeSpatialCurve(const SpatialCurve& curve)
{
    if (curve.points.size() < (curve.closed ? 3u : 2u) || curve.points.size() > 512) {
        throw std::invalid_argument("Use two to 512 distinct points, or at least three for a closed curve.");
    }
    Handle(TColgp_HArray1OfPnt) points = new TColgp_HArray1OfPnt(1, static_cast<int>(curve.points.size()));
    BRepBuilderAPI_MakePolygon polygon;
    for (int i = 1; i <= points->Length(); ++i) {
        const auto& point = curve.points[i - 1];
        if (!std::ranges::all_of(point, [](double c) { return std::isfinite(c) && std::abs(c) <= 1.e6; })) {
            throw std::invalid_argument("Spatial curve coordinates must be within ±1,000,000 mm.");
        }
        const gp_Pnt position(point[0], point[1], point[2]);
        if ((i > 1 && position.Distance(points->Value(i - 1)) <= tolerance)
            || (curve.closed && i == points->Length() && position.Distance(points->Value(1)) <= tolerance)) {
            throw std::invalid_argument("Consecutive curve points must be distinct.");
        }
        points->SetValue(i, position);
        if (!curve.spline) { polygon.Add(position); }
    }
    if (!curve.spline) {
        if (curve.closed) { polygon.Close(); }
        return checked(polygon.Wire(), "Could not build the spatial polyline.");
    }
    GeomAPI_Interpolate interpolation(points, curve.closed, tolerance);
    interpolation.Perform();
    if (!interpolation.IsDone()) { throw std::invalid_argument("Could not interpolate the spatial spline."); }
    return checked(BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(interpolation.Curve()).Edge()).Wire(),
                   "Could not build the spatial spline.");
}

TopoDS_Shape buildSurface(SurfaceKind kind, std::span<const TopoDS_Shape> shapes, double distance, int index)
{
    if (shapes.empty() || shapes.size() > 32
        || std::ranges::any_of(shapes, [](const auto& shape) { return shape.IsNull(); })) {
        throw std::invalid_argument("Select valid inputs for the surface operation.");
    }
    const auto& first = shapes.front();
    switch (kind) {
    case SurfaceKind::Loft: {
        if (shapes.size() < 2) { break; }
        BRepOffsetAPI_ThruSections builder(false, index != 0, tolerance);
        for (const auto& shape : shapes) { builder.AddWire(wire(shape)); }
        builder.Build();
        if (builder.IsDone()) { return checked(builder.Shape(), "Loft did not create a valid surface."); }
        throw std::invalid_argument("Could not loft the selected sections.");
    }
    case SurfaceKind::Sweep: {
        if (shapes.size() != 2) { break; }
        BRepOffsetAPI_MakePipeShell sweep(wire(shapes[1]));
        sweep.Add(wire(first), false, false);
        sweep.Build();
        if (sweep.IsDone()) { return checked(sweep.Shape(), "Sweep did not create a valid surface."); }
        throw std::invalid_argument("Could not sweep this section along the path.");
    }
    case SurfaceKind::Network:
        if (index < 1 || static_cast<std::size_t>(index) > shapes.size()) { break; }
        return fill(shapes, index);
    case SurfaceKind::Patch: {
        if (shapes.size() != 1 || index < 1) { break; }
        const auto boundary = TopExp_Explorer(first, TopAbs_FACE).More()
            ? wire(openEdges(first, true, index)) : wire(first);
        if (!BRep_Tool::IsClosed(boundary)) { throw std::invalid_argument("Patch requires a closed boundary."); }
        BRepBuilderAPI_MakeFace planar(boundary, true);
        if (planar.IsDone()) { return checked(planar.Face(), "The planar patch is invalid."); }
        const std::array<TopoDS_Shape,1> curves{boundary};
        return fill(curves, 1);
    }
    case SurfaceKind::Trim: {
        if (shapes.size() != 2 || index < 1) { break; }
        TopExp_Explorer faces(first, TopAbs_FACE);
        if (!faces.More()) { break; }
        faces.Next();
        if (faces.More()) { throw std::invalid_argument("Trim one face at a time."); }
        TopTools_ListOfShape targets, cutters;
        targets.Append(first);
        cutters.Append(shapes[1]);
        BRepAlgoAPI_Splitter split;
        split.SetArguments(targets);
        split.SetTools(cutters);
        split.Build();
        if (!split.IsDone() || split.HasErrors()) { throw std::invalid_argument("The cutter could not split the surface."); }
        std::vector<TopoDS_Shape> pieces;
        for (TopExp_Explorer faces(split.Shape(), TopAbs_FACE); faces.More(); faces.Next()) { pieces.push_back(faces.Current()); }
        if (pieces.size() < 2 || static_cast<std::size_t>(index) > pieces.size()) {
            throw std::invalid_argument("The cutter must split the surface into the requested pieces.");
        }
        return checked(pieces[index - 1], "The trimmed surface is invalid.");
    }
    case SurfaceKind::Extend: {
        if (shapes.size() != 1 || index < 1 || index > 4 || !std::isfinite(distance) || distance <= 0 || distance > 1.e5) { break; }
        TopExp_Explorer faces(first, TopAbs_FACE);
        if (!faces.More()) { break; }
        const auto face = TopoDS::Face(faces.Current());
        faces.Next();
        if (faces.More()) { throw std::invalid_argument("Extension requires exactly one face."); }
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        Handle(Geom_RectangularTrimmedSurface) window = new Geom_RectangularTrimmedSurface(
            BRep_Tool::Surface(face), u0, u1, v0, v1);
        const auto untrimmed = BRepBuilderAPI_MakeFace(window, tolerance).Face();
        GProp_GProps actualArea, originalArea;
        BRepGProp::SurfaceProperties(face, actualArea);
        BRepGProp::SurfaceProperties(untrimmed, originalArea);
        if (std::abs(actualArea.Mass() - originalArea.Mass()) > std::max(1.e-6, originalArea.Mass() * 1.e-6)) {
            throw std::invalid_argument("Extension requires a rectangular parameter boundary without holes.");
        }
        Handle(Geom_BoundedSurface) extended = GeomConvert::SurfaceToBSplineSurface(window);
        const bool inU = index <= 2;
        if (inU ? extended->IsUPeriodic() : extended->IsVPeriodic()) {
            throw std::invalid_argument("Select a nonperiodic direction for extension.");
        }
        GeomLib::ExtendSurfByLength(extended, distance, 1, inU, index % 2 == 0);
        BRepBuilderAPI_MakeFace builder(extended, tolerance);
        if (builder.IsDone()) { return checked(builder.Face(), "The extended surface is invalid."); }
        throw std::invalid_argument("Could not extend the selected surface boundary.");
    }
    case SurfaceKind::Sew: return sewn(shapes, distance, false);
    case SurfaceKind::Solid: return sewn(shapes, distance, true);
    case SurfaceKind::OpenEdges:
        if (shapes.size() == 1) { return openEdges(first, false, 0); }
        break;
    }
    throw std::invalid_argument("Invalid surface operation inputs or parameters.");
}

}  // namespace graph3d::geometry
