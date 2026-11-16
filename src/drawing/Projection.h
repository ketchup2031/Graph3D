#pragma once

#include <array>
#include <vector>

class TopoDS_Shape;

namespace graph3d::drawing {

struct Point { double x; double y; };
using Linework = std::vector<std::vector<Point>>;
struct RoundContour { Point center; double radius; };

struct View {
    Linework visible;
    Linework hidden;
    double width;
    double height;
    std::vector<RoundContour> circles;
};

struct Projections {
    View front;
    View top;
    View right;
};

Projections project(const TopoDS_Shape& solid);

}  // namespace graph3d::drawing
