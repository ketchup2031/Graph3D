#pragma once

#include "geometry/Sketch.h"
#include <vector>
#include <array>

#include <QUuid>

class TopoDS_Shape;

namespace graph3d::drawing {

struct Point { double x = 0; double y = 0; bool operator==(const Point&) const = default; };
using Linework = std::vector<std::vector<Point>>;
struct RoundContour { Point center; double radius; };

struct ProjectedEdge {
    QUuid id;
    Point a;
    Point b;
    double radius = 0;
    double angle = 0;
    double sweep = 0;
};

struct View {
    Linework visible;
    Linework hidden;
    double width;
    double height;
    std::vector<RoundContour> circles;
    QUuid id;
    Point origin;
    std::vector<ProjectedEdge> edges;
};

struct Projections {
    View front;
    View top;
    View right;
};

struct ViewDirection {
    geometry::Point3 normal{0, -1, 0};
    geometry::Point3 right{1, 0, 0};
};

struct CutProjection {
    View view;
    std::vector<Linework> material;
};

struct Section {
    geometry::Plane plane;
    double offset;
    View view;
    std::vector<Linework> material;
};

Projections project(const TopoDS_Shape& solid);
void retainProjectionReferences(View& view, const View& previous);
void uniqueProjectionEdges(View& view);
ViewDirection standardDirection(int index);
ViewDirection angledDirection(double yaw, double pitch, double roll);
ViewDirection projectedDirection(const ViewDirection& source, Point direction);
View projectOriented(const TopoDS_Shape& solid, const ViewDirection& direction);
CutProjection projectLocalCut(const TopoDS_Shape& solid, const ViewDirection& direction, double depth);
CutProjection projectCut(const TopoDS_Shape& solid, const ViewDirection& source,
                         std::span<const Point> path, bool aligned);
std::vector<std::array<Point, 3>> componentViewCenters(const TopoDS_Shape& assembly);
Section section(const TopoDS_Shape& solid, geometry::Plane plane, double offset);

}  // namespace graph3d::drawing
