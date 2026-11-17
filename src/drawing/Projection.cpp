#include "drawing/Projection.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <BRepBndLib.hxx>
#include <BRepLib.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace graph3d::drawing {
namespace {

struct CircleCoverage {
    RoundContour contour;
    std::vector<std::pair<double, double>> intervals;
};

constexpr double turn = 2 * std::numbers::pi;
constexpr double angularTolerance = 1.e-5;

void addCircle(std::vector<CircleCoverage>& coverage, const BRepAdaptor_Curve& curve, double tolerance)
{
    if (curve.GetType() != GeomAbs_Circle) {
        return;
    }
    const auto circle = curve.Circle();
    const auto center = circle.Location();
    const auto axis = circle.Axis().Direction().Z();
    if (std::abs(axis) < 1 - angularTolerance) {
        return;
    }
    const auto position = Point{center.X(), center.Y()};
    const auto radius = circle.Radius();
    const auto span = curve.LastParameter() - curve.FirstParameter();
    if (!(span > 0 && radius > tolerance)) {
        return;
    }
    const auto same = [position, radius, tolerance](const CircleCoverage& entry) {
        return std::hypot(entry.contour.center.x - position.x, entry.contour.center.y - position.y)
                   <= tolerance
            && std::abs(entry.contour.radius - radius) <= tolerance;
    };
    auto match = std::find_if(coverage.begin(), coverage.end(), same);
    if (match == coverage.end()) {
        coverage.push_back({{position, radius}, {}});
        match = std::prev(coverage.end());
    }
    if (span >= turn - angularTolerance) {
        match->intervals.emplace_back(0, turn);
        return;
    }
    const auto point = curve.Value(axis > 0 ? curve.FirstParameter() : curve.LastParameter());
    const auto angle = std::atan2(point.Y() - position.y, point.X() - position.x);
    const auto start = angle < 0 ? angle + turn : angle;
    const auto end = start + span;
    match->intervals.emplace_back(start, std::min(end, turn));
    if (end > turn) {
        match->intervals.emplace_back(0, end - turn);
    }
}

bool isComplete(CircleCoverage& coverage)
{
    std::sort(coverage.intervals.begin(), coverage.intervals.end());
    double end = 0;
    for (const auto& [start, stop] : coverage.intervals) {
        if (start > end + angularTolerance) {
            return false;
        }
        end = std::max(end, stop);
    }
    return end >= turn - angularTolerance;
}

std::vector<Point> sampleCurve(const BRepAdaptor_Curve& curve, double tolerance, const gp_Ax2* frame = nullptr)
{
    const auto first = curve.FirstParameter();
    const auto last = curve.LastParameter();
    std::vector<Point> points;
    const auto append = [&](const gp_Pnt& vertex) {
        Point point{vertex.X(), vertex.Y()};
        if (frame) {
            const gp_Vec relative(frame->Location(), vertex);
            point = {relative.Dot(gp_Vec(frame->XDirection())), relative.Dot(gp_Vec(frame->YDirection()))};
        }
        if (points.empty() || points.back().x != point.x || points.back().y != point.y) {
            points.push_back(point);
        }
    };
    if (curve.GetType() == GeomAbs_Line) {
        append(curve.Value(first));
        append(curve.Value(last));
    } else {
        GCPnts_QuasiUniformDeflection samples(curve, tolerance, first, last);
        if (!samples.IsDone()) {
            throw std::runtime_error("Could not sample a curved drawing edge.");
        }
        points.reserve(static_cast<std::size_t>(samples.NbPoints()));
        for (int i = 1; i <= samples.NbPoints(); ++i) {
            append(samples.Value(i));
        }
    }
    return points;
}

void appendEdges(Linework& lines, const TopoDS_Shape& edges, double tolerance,
                 std::vector<CircleCoverage>* circles = nullptr)
{
    if (edges.IsNull()) {
        return;
    }
    if (!BRepLib::BuildCurves3d(edges)) {
        throw std::runtime_error("Could not build the curves of the projection.");
    }
    for (TopExp_Explorer it(edges, TopAbs_EDGE); it.More(); it.Next()) {
        const BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));
        if (circles) {
            addCircle(*circles, curve, std::max(1.e-8, tolerance * 0.001));
        }
        auto points = sampleCurve(curve, tolerance);
        if (points.size() > 1) {
            lines.push_back(std::move(points));
        }
    }
}

View makeView(const TopoDS_Shape& solid, const gp_Ax2& placement, double width, double height)
{
    const Handle(HLRBRep_Algo) algorithm = new HLRBRep_Algo;
    algorithm->Add(solid, 0);
    algorithm->Projector(HLRAlgo_Projector(placement));
    algorithm->Update();
    algorithm->Hide();

    HLRBRep_HLRToShape edges(algorithm);
    View view{{}, {}, width, height};
    // Approximate only the display of curves, to less than 0.02 mm on the A4 drawing.
    const double tolerance = std::max(width, height) * 0.00015;
    std::vector<CircleCoverage> circles;
    appendEdges(view.visible, edges.VCompound(), tolerance, &circles);
    appendEdges(view.visible, edges.OutLineVCompound(), tolerance, &circles);
    for (auto& circle : circles) {
        if (isComplete(circle)) {
            view.circles.push_back(circle.contour);
        }
    }
    appendEdges(view.hidden, edges.HCompound(), tolerance);
    appendEdges(view.hidden, edges.OutLineHCompound(), tolerance);
    if (view.visible.empty()) {
        throw std::runtime_error("No visible edges were found for an orthographic view.");
    }
    return view;
}

struct Bounds {
    gp_Pnt low;
    gp_Pnt high;
    gp_Pnt center;
    double x, y, z;
};

Bounds boundsOf(const TopoDS_Shape& solid)
{
    if (solid.IsNull() || !TopExp_Explorer(solid, TopAbs_SOLID).More()) {
        throw std::invalid_argument("Select a solid body to create a drawing.");
    }
    Bnd_Box bounds;
    BRepBndLib::AddOptimal(solid, bounds, false, false);
    if (bounds.IsVoid() || bounds.IsOpen()) {
        throw std::invalid_argument("The body has no finite drawing bounds.");
    }
    const auto low = bounds.CornerMin();
    const auto high = bounds.CornerMax();
    const double x = high.X() - low.X();
    const double y = high.Y() - low.Y();
    const double z = high.Z() - low.Z();
    if (!(x > 0 && y > 0 && z > 0) || !std::isfinite(std::max({x, y, z}))) {
        throw std::invalid_argument("The body has invalid dimensions.");
    }
    const gp_Pnt center(low.X() + x / 2, low.Y() + y / 2, low.Z() + z / 2);
    return {low, high, center, x, y, z};
}

struct Frame {
    gp_Ax2 axes;
    double width;
    double height;
    int normalAxis;
};

Frame frameOf(const Bounds& bounds, geometry::Plane plane)
{
    switch (plane) {
    case geometry::Plane::XZ:
        return {gp_Ax2(bounds.center, gp_Dir(0, -1, 0), gp_Dir(1, 0, 0)), bounds.x, bounds.z, 2};
    case geometry::Plane::XY:
        return {gp_Ax2(bounds.center, gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)), bounds.x, bounds.y, 3};
    case geometry::Plane::YZ:
        return {gp_Ax2(bounds.center, gp_Dir(1, 0, 0), gp_Dir(0, 1, 0)), bounds.y, bounds.z, 1};
    }
    throw std::invalid_argument("Choose an XY, XZ, or YZ section plane.");
}

Linework faceLoops(const TopoDS_Face& face, const Frame& frame)
{
    Linework loops;
    const double tolerance = std::max(frame.width, frame.height) * 0.00015;
    for (TopExp_Explorer wires(face, TopAbs_WIRE); wires.More(); wires.Next()) {
        std::vector<Point> loop;
        for (BRepTools_WireExplorer edges(TopoDS::Wire(wires.Current()), face); edges.More(); edges.Next()) {
            const auto& edge = edges.Current();
            if (BRep_Tool::Degenerated(edge)) {
                continue;
            }
            auto points = sampleCurve(BRepAdaptor_Curve(edge), tolerance, &frame.axes);
            if (edges.Orientation() == TopAbs_REVERSED) {
                std::reverse(points.begin(), points.end());
            }
            loop.insert(loop.end(), points.begin(), points.end());
        }
        if (loop.size() < 3 || std::hypot(loop.front().x - loop.back().x, loop.front().y - loop.back().y)
                                   > std::max(tolerance, Precision::Confusion())) {
            throw std::runtime_error("A section boundary could not be closed.");
        }
        loops.push_back(std::move(loop));
    }
    return loops;
}

}  // namespace

std::vector<std::array<Point, 3>> componentViewCenters(const TopoDS_Shape& assembly)
{
    const auto origin = boundsOf(assembly).center;
    std::vector<std::array<Point, 3>> result;
    for (TopoDS_Iterator component(assembly); component.More(); component.Next()) {
        const auto center = boundsOf(component.Value()).center;
        const auto x = center.X() - origin.X();
        const auto y = center.Y() - origin.Y();
        const auto z = center.Z() - origin.Z();
        result.push_back({Point{x, -z}, Point{y, -z}, Point{x, -y}});
    }
    return result;
}

Projections project(const TopoDS_Shape& solid)
{
    try {
        const auto bounds = boundsOf(solid);
        const auto view = [&](geometry::Plane plane) {
            const auto frame = frameOf(bounds, plane);
            return makeView(solid, frame.axes, frame.width, frame.height);
        };
        return {view(geometry::Plane::XZ), view(geometry::Plane::XY), view(geometry::Plane::YZ)};
    } catch (const Standard_Failure& failure) {
        const auto* message = failure.GetMessageString();
        throw std::runtime_error(message && *message ? message : "Open CASCADE could not compute the projections.");
    }
}

Section section(const TopoDS_Shape& solid, geometry::Plane plane, double offset)
{
    try {
        const auto bounds = boundsOf(solid);
        const auto frame = frameOf(bounds, plane);
        const int axis = frame.normalAxis;
        const double coordinate = bounds.center.Coord(axis) + offset;
        constexpr double tolerance = 5 * Precision::Confusion();
        if (!std::isfinite(coordinate) || coordinate <= bounds.low.Coord(axis) + tolerance
            || coordinate >= bounds.high.Coord(axis) - tolerance) {
            throw std::invalid_argument("Move the section plane inside the body bounds.");
        }

        const double margin = std::max({bounds.x, bounds.y, bounds.z}) * 0.1;
        const gp_Vec padding(margin, margin, margin);
        auto low = bounds.low.Translated(-padding);
        auto high = bounds.high.Translated(padding);
        // Keep the half opposite the viewer, using the same viewing direction as the original view.
        if (frame.axes.Direction().Coord(axis) > 0) {
            high.SetCoord(axis, coordinate);
        } else {
            low.SetCoord(axis, coordinate);
        }
        const auto half = BRepPrimAPI_MakeBox(low, high).Shape();
        BRepAlgoAPI_Common clip(solid, half);
        if (!clip.IsDone() || clip.HasErrors() || clip.Shape().IsNull()) {
            throw std::runtime_error("Could not cut the body for the section view.");
        }
        const auto& clipped = clip.Shape();
        auto pointOnCut = bounds.center;
        pointOnCut.SetCoord(axis, coordinate);
        std::vector<Linework> material;
        for (TopExp_Explorer faces(clipped, TopAbs_FACE); faces.More(); faces.Next()) {
            const auto face = TopoDS::Face(faces.Current());
            const BRepAdaptor_Surface surface(face);
            if (surface.GetType() != GeomAbs_Plane) {
                continue;
            }
            const auto surfacePlane = surface.Plane();
            if (surfacePlane.Distance(pointOnCut) <= tolerance
                && surfacePlane.Axis().Direction().IsParallel(frame.axes.Direction(), angularTolerance)) {
                auto loops = faceLoops(face, frame);
                if (!loops.empty()) {
                    material.push_back(std::move(loops));
                }
            }
        }
        if (material.empty()) {
            throw std::invalid_argument("The section plane does not cross material at this offset.");
        }
        auto view = makeView(clipped, frame.axes, frame.width, frame.height);
        return {plane, offset, std::move(view), std::move(material)};
    } catch (const Standard_Failure& failure) {
        const auto* message = failure.GetMessageString();
        throw std::runtime_error(message && *message ? message : "Open CASCADE could not compute the section.");
    }
}

}  // namespace graph3d::drawing
