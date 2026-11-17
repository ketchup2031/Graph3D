#include "drawing/Projection.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
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
#include <gp_Ax3.hxx>
#include <QPointF>
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

void describeEdge(std::vector<ProjectedEdge>& edges, const BRepAdaptor_Curve& curve,
                  const std::vector<Point>& points)
{
    if (curve.GetType() == GeomAbs_Line) {
        auto a = points.front();
        auto b = points.back();
        const bool horizontal = std::abs(b.x - a.x) >= std::abs(b.y - a.y);
        if ((horizontal && b.x < a.x) || (!horizontal && b.y < a.y)) {
            std::swap(a, b);
        }
        edges.push_back({QUuid::createUuid(), a, b});
    } else if (curve.GetType() == GeomAbs_Circle) {
        const auto circle = curve.Circle();
        const auto center = circle.Location();
        const auto span = curve.LastParameter() - curve.FirstParameter();
        const bool complete = span >= turn - angularTolerance;
        const auto begin = circle.Axis().Direction().Z() > 0 ? points.front() : points.back();
        const double angle = complete ? 0 : std::atan2(begin.y - center.Y(), begin.x - center.X());
        edges.push_back({QUuid::createUuid(), {center.X(), center.Y()}, {}, circle.Radius(), angle,
                         complete ? turn : span});
    }
}

void appendEdges(Linework& lines, const TopoDS_Shape& edges, double tolerance,
                 std::vector<CircleCoverage>* circles = nullptr, std::vector<ProjectedEdge>* references = nullptr)
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
            if (references) {
                describeEdge(*references, curve, points);
            }
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
    view.id = QUuid::createUuid();
    const gp_Vec origin(placement.Location().XYZ());
    view.origin = {origin.Dot(gp_Vec(placement.XDirection())), origin.Dot(gp_Vec(placement.YDirection()))};
    // Approximate only the display of curves, to less than 0.02 mm on the A4 drawing.
    const double tolerance = std::max(width, height) * 0.00015;
    std::vector<CircleCoverage> circles;
    appendEdges(view.visible, edges.VCompound(), tolerance, &circles, &view.edges);
    appendEdges(view.visible, edges.OutLineVCompound(), tolerance, &circles, &view.edges);
    uniqueProjectionEdges(view);
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

ViewDirection standardDirection(int index)
{
    switch (index) {
    case 0: return {{0, -1, 0}, {1, 0, 0}};
    case 1: return {{1, 0, 0}, {0, 1, 0}};
    case 2: return {{0, 0, 1}, {1, 0, 0}};
    case 3: return {{0, 1, 0}, {-1, 0, 0}};
    case 4: return {{-1, 0, 0}, {0, -1, 0}};
    case 5: return {{0, 0, -1}, {1, 0, 0}};
    default: throw std::invalid_argument("Invalid standard view.");
    }
}

namespace {

gp_Dir directionOf(const geometry::Point3& v)
{
    return {v[0], v[1], v[2]};
}

geometry::Point3 directionOf(const gp_Dir& v)
{
    return {v.X(), v.Y(), v.Z()};
}

std::pair<double, double> projectedSize(const Bounds& bounds, const gp_Ax2& axes)
{
    const auto span = [&](const auto& dir) {
        return std::abs(dir.X()) * bounds.x + std::abs(dir.Y()) * bounds.y + std::abs(dir.Z()) * bounds.z;
    };
    return {span(axes.XDirection()), span(axes.YDirection())};
}

}  // namespace

ViewDirection angledDirection(double yaw, double pitch, double roll)
{
    constexpr double radians = std::numbers::pi / 180;
    yaw *= radians;
    pitch *= radians;
    roll *= radians;
    const gp_Dir normal(std::sin(yaw) * std::cos(pitch), -std::cos(yaw) * std::cos(pitch), std::sin(pitch));
    const gp_Dir right(std::cos(yaw), std::sin(yaw), 0);
    const auto up = normal.Crossed(right);
    const gp_Dir rolled(gp_Vec(right) * std::cos(roll) + gp_Vec(up) * std::sin(roll));
    return {directionOf(normal), directionOf(rolled)};
}

ViewDirection projectedDirection(const ViewDirection& source, Point vector)
{
    const auto sourceNormal = directionOf(source.normal);
    const auto right = directionOf(source.right);
    const auto up = sourceNormal.Crossed(right);
    const gp_Dir normal(gp_Vec(right) * vector.x + gp_Vec(up) * vector.y);
    return {directionOf(normal), directionOf(sourceNormal.Crossed(normal))};
}

View projectOriented(const TopoDS_Shape& solid, const ViewDirection& direction)
{
    try {
        const auto bounds = boundsOf(solid);
        const gp_Ax2 axes(bounds.center, directionOf(direction.normal), directionOf(direction.right));
        const auto [width, height] = projectedSize(bounds, axes);
        return makeView(solid, axes, width, height);
    } catch (const Standard_Failure& failure) {
        throw std::runtime_error(failure.GetMessageString() ? failure.GetMessageString()
                                                          : "Could not project the model in this direction.");
    }
}

namespace {

double dot(QPointF a, QPointF b)
{
    return QPointF::dotProduct(a, b);
}

QPointF unit(QPointF v)
{
    const auto length = std::hypot(v.x(), v.y());
    if (!(length >= 1.e-5)) {
        throw std::invalid_argument("Cut path points must be distinct.");
    }
    return v / length;
}

QPointF left(QPointF direction)
{
    return {-direction.y(), direction.x()};
}

using Polygon = std::vector<QPointF>;

Polygon clipPolygon(const Polygon& polygon, QPointF origin, QPointF inward)
{
    Polygon result;
    if (polygon.empty()) {
        return result;
    }
    auto previous = polygon.back();
    double last = dot(previous - origin, inward);
    for (const auto point : polygon) {
        const double next = dot(point - origin, inward);
        if ((last >= 0) != (next >= 0)) {
            result.push_back(previous + (point - previous) * (last / (last - next)));
        }
        if (next >= 0) {
            result.push_back(point);
        }
        previous = point;
        last = next;
    }
    return result;
}

TopoDS_Shape clipSolid(const TopoDS_Shape& solid, const Polygon& polygon, double depth)
{
    if (polygon.size() < 3) {
        return {};
    }
    BRepBuilderAPI_MakePolygon wire;
    for (auto p : polygon) {
        wire.Add(gp_Pnt(p.x(), p.y(), -depth));
    }
    wire.Close();
    BRepBuilderAPI_MakeFace face(wire.Wire());
    if (!face.IsDone()) {
        throw std::invalid_argument("The cut path does not enclose a valid cutting region.");
    }
    const auto prism = BRepPrimAPI_MakePrism(face.Face(), gp_Vec(0, 0, 2 * depth)).Shape();
    BRepAlgoAPI_Common clipped(solid, prism);
    if (!clipped.IsDone() || clipped.HasErrors()) {
        throw std::runtime_error("Could not cut the body along this path.");
    }
    return clipped.Shape();
}

struct CutSpan {
    QPointF start;
    QPointF end;
    QPointF tangent;
};

void appendCut(CutProjection& result, const TopoDS_Shape& clipped, const Bounds& bounds,
               std::span<const CutSpan> spans, QPointF origin, QPointF tangent, double shift,
               double faceWidth, bool unfolded)
{
    if (clipped.IsNull() || !TopExp_Explorer(clipped, TopAbs_SOLID).More()) {
        return;
    }
    const auto inward = left(tangent);
    const gp_Ax2 axes(gp_Pnt(origin.x(), origin.y(), 0), gp_Dir(-inward.x(), -inward.y(), 0),
                      gp_Dir(tangent.x(), tangent.y(), 0));
    const Frame frame{axes, faceWidth, bounds.z, 0};
    constexpr double tolerance = 1.e-5;
    for (TopExp_Explorer faces(clipped, TopAbs_FACE); faces.More(); faces.Next()) {
        const auto face = TopoDS::Face(faces.Current());
        const BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane) {
            continue;
        }
        const auto plane = surface.Plane();
        for (const auto& span : spans) {
            const auto side = left(span.tangent);
            if (plane.Distance(gp_Pnt(span.start.x(), span.start.y(), 0)) > tolerance
                || !plane.Axis().Direction().IsParallel(gp_Dir(side.x(), side.y(), 0), angularTolerance)) {
                continue;
            }
            auto loops = faceLoops(face, frame);
            for (auto& loop : loops) {
                for (auto& point : loop) {
                    point.x += shift;
                }
            }
            if (!loops.empty()) {
                result.material.push_back(std::move(loops));
            }
            break;
        }
    }

    auto view = makeView(clipped, axes, faceWidth, bounds.z);
    for (auto& line : view.visible) {
        for (auto& point : line) {
            point.x += shift;
        }
        result.view.visible.push_back(std::move(line));
    }
    for (auto edge : view.edges) {
        edge.a.x += shift;
        edge.b.x += shift;
        result.view.edges.push_back(edge);
    }
    for (auto circle : view.circles) {
        circle.center.x += shift;
        result.view.circles.push_back(circle);
    }
    if (!unfolded) {
        result.view.origin = view.origin;
    }
}

}  // namespace

CutProjection projectLocalCut(const TopoDS_Shape& solid, const ViewDirection& direction, double depth)
{
    try {
        const auto original = boundsOf(solid);
        gp_Trsf transform;
        transform.SetTransformation(gp_Ax3(original.center, directionOf(direction.normal), directionOf(direction.right)));
        const auto local = BRepBuilderAPI_Transform(solid, transform, false).Shape();
        const auto bounds = boundsOf(local);
        constexpr double epsilon = 5 * Precision::Confusion();
        if (!std::isfinite(depth) || depth <= bounds.low.Z() + epsilon || depth >= bounds.high.Z() - epsilon) {
            throw std::invalid_argument("Move the section plane inside the body bounds.");
        }
        const auto margin = std::max({bounds.x, bounds.y, bounds.z}) * .1;
        const auto low = bounds.low.Translated(gp_Vec(-margin, -margin, -margin));
        auto high = bounds.high.Translated(gp_Vec(margin, margin, margin));
        high.SetZ(depth);
        BRepAlgoAPI_Common clip(local, BRepPrimAPI_MakeBox(low, high).Shape());
        if (!clip.IsDone() || clip.HasErrors() || clip.Shape().IsNull()) {
            throw std::runtime_error("Could not cut the body for the section view.");
        }
        const Frame frame{gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)), bounds.x, bounds.y, 3};
        CutProjection result;
        for (TopExp_Explorer it(clip.Shape(), TopAbs_FACE); it.More(); it.Next()) {
            const auto face = TopoDS::Face(it.Current());
            const BRepAdaptor_Surface surface(face);
            if (surface.GetType() == GeomAbs_Plane) {
                const auto plane = surface.Plane();
                if (plane.Distance(gp_Pnt(0, 0, depth)) <= epsilon
                    && plane.Axis().Direction().IsParallel(frame.axes.Direction(), angularTolerance)) {
                    auto loops = faceLoops(face, frame);
                    if (!loops.empty()) {
                        result.material.push_back(std::move(loops));
                    }
                }
            }
        }
        if (result.material.empty()) {
            throw std::invalid_argument("The section plane does not cross material at this offset.");
        }
        result.view = makeView(clip.Shape(), frame.axes, frame.width, frame.height);
        return result;
    } catch (const Standard_Failure& failure) {
        throw std::runtime_error(failure.GetMessageString() ? failure.GetMessageString()
                                                          : "Open CASCADE could not compute the local section.");
    }
}

CutProjection projectCut(const TopoDS_Shape& solid, const ViewDirection& source,
                         std::span<const Point> path, bool aligned)
{
    try {
        if (path.size() < (aligned ? 3u : 4u) || path.size() > 16) {
            throw std::invalid_argument("Enter three to sixteen points for an aligned section or four to sixteen for a stepped section.");
        }
        const auto original = boundsOf(solid);
        gp_Trsf transform;
        transform.SetTransformation(gp_Ax3(original.center, directionOf(source.normal), directionOf(source.right)));
        const auto local = BRepBuilderAPI_Transform(solid, transform, false).Shape();
        const auto bounds = boundsOf(local);
        std::vector<CutSpan> spans;
        for (std::size_t i = 1; i < path.size(); ++i) {
            const QPointF previous(path[i - 1].x, path[i - 1].y);
            const QPointF next(path[i].x, path[i].y);
            spans.push_back({previous, next, unit(next - previous)});
        }
        const auto first = spans.front().tangent;
        const auto last = spans.back().tangent;
        double extent = 2 * std::max({bounds.x, bounds.y, bounds.z});
        for (auto point : path) {
            extent = std::max(extent, 2 * std::hypot(point.x, point.y));
        }
        CutProjection result;
        result.view.id = QUuid::createUuid();
        result.view.height = bounds.z;
        if (aligned) {
            const Polygon field{{-extent, -extent}, {extent, -extent}, {extent, extent}, {-extent, extent}};
            double shift = 0;
            for (std::size_t i = 0; i < spans.size(); ++i) {
                const auto& span = spans[i];
                auto polygon = clipPolygon(field, span.start, left(span.tangent));
                if (i > 0) {
                    polygon = clipPolygon(polygon, span.start, unit(spans[i - 1].tangent + span.tangent));
                }
                if (i + 1 < spans.size()) {
                    polygon = clipPolygon(polygon, span.end, -unit(span.tangent + spans[i + 1].tangent));
                }
                const auto clipped = clipSolid(local, polygon, extent);
                appendCut(result, clipped, bounds, std::span(&span, 1), span.start, span.tangent, shift, extent, true);
                shift += std::hypot(span.end.x() - span.start.x(), span.end.y() - span.start.y());
            }
            if (!result.view.visible.empty()) {
                double low = result.view.visible.front().front().x;
                double high = low;
                for (const auto& line : result.view.visible) {
                    for (const auto& point : line) {
                        low = std::min(low, point.x);
                        high = std::max(high, point.x);
                    }
                }
                const double center = (high + low) / 2;
                for (auto& line : result.view.visible) {
                    for (auto& point : line) {
                        point.x -= center;
                    }
                }
                for (auto& face : result.material) {
                    for (auto& loop : face) {
                        for (auto& point : loop) {
                            point.x -= center;
                        }
                    }
                }
                for (auto& edge : result.view.edges) {
                    edge.a.x -= center;
                    edge.b.x -= center;
                }
                for (auto& circle : result.view.circles) {
                    circle.center.x -= center;
                }
                result.view.origin.x = center;
                result.view.width = high - low;
            }
        } else {
            if (dot(first, last) < 1 - angularTolerance || std::ranges::any_of(spans, [&](const auto& span) {
                    const auto alignment = dot(span.tangent, first);
                    return std::abs(alignment) > angularTolerance && alignment < 1 - angularTolerance;
                })) {
                throw std::invalid_argument("Use parallel legs and perpendicular jogs for a stepped section.");
            }
            const auto begin = spans.front().start - first * extent;
            const auto end = spans.back().end + first * extent;
            Polygon polygon{begin};
            for (const auto point : path) {
                polygon.emplace_back(point.x, point.y);
            }
            polygon.push_back(end);
            polygon.push_back(end + left(first) * 2 * extent);
            polygon.push_back(begin + left(first) * 2 * extent);
            const auto clipped = clipSolid(local, polygon, extent);
            std::vector<CutSpan> parallel;
            std::ranges::copy_if(spans, std::back_inserter(parallel), [&](const auto& span) {
                return dot(span.tangent, first) > 1 - angularTolerance;
            });
            result.view.width = bounds.x * std::abs(first.x()) + bounds.y * std::abs(first.y());
            appendCut(result, clipped, bounds, parallel, {}, first, 0, result.view.width, false);
        }
        if (result.view.visible.empty() || result.material.empty() || !(result.view.width > 0)) {
            throw std::invalid_argument("The section path does not cross material.");
        }
        uniqueProjectionEdges(result.view);
        return result;
    } catch (const Standard_Failure& failure) {
        throw std::runtime_error(failure.GetMessageString() ? failure.GetMessageString()
                                                          : "Open CASCADE could not compute the section path.");
    }
}

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
