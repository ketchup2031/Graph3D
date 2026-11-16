#include "drawing/Projection.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepLib.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

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
        const auto first = curve.FirstParameter();
        const auto last = curve.LastParameter();
        std::vector<Point> points;
        const auto append = [&points](const gp_Pnt& vertex) {
            if (points.empty() || points.back().x != vertex.X() || points.back().y != vertex.Y()) {
                points.push_back({vertex.X(), vertex.Y()});
            }
        };
        if (curve.GetType() == GeomAbs_Line) {
            append(curve.Value(first));
            append(curve.Value(last));
        } else {
            GCPnts_QuasiUniformDeflection samples(curve, tolerance, first, last);
            if (!samples.IsDone()) {
                throw std::runtime_error("Could not sample a curved projection edge.");
            }
            points.reserve(static_cast<std::size_t>(samples.NbPoints()));
            for (int i = 1; i <= samples.NbPoints(); ++i) {
                append(samples.Value(i));
            }
        }
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

}  // namespace

Projections project(const TopoDS_Shape& solid)
{
    try {
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
        const gp_Pnt center((low.X() + high.X()) / 2, (low.Y() + high.Y()) / 2,
                            (low.Z() + high.Z()) / 2);
        return {
            makeView(solid, gp_Ax2(center, gp_Dir(0, -1, 0), gp_Dir(1, 0, 0)), x, z),
            makeView(solid, gp_Ax2(center, gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)), x, y),
            makeView(solid, gp_Ax2(center, gp_Dir(1, 0, 0), gp_Dir(0, 1, 0)), y, z)
        };
    } catch (const Standard_Failure& failure) {
        const auto* message = failure.GetMessageString();
        throw std::runtime_error(message && *message ? message : "Open CASCADE could not compute the projections.");
    }
}

}  // namespace graph3d::drawing
