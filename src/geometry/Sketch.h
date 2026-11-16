#pragma once

#include <variant>
#include <vector>

namespace graph3d::geometry {

enum class Plane { XY, XZ, YZ };
enum class Axis { U, V };
enum class BooleanOperation { Union, Difference, Intersection };

struct Point2 {
    double u;
    double v;
};

struct Line { Point2 start, end; };
struct Rectangle { Point2 first, opposite; };
struct Circle { Point2 center; double radius; };
using SketchEntity = std::variant<Line, Rectangle, Circle>;

struct Sketch {
    Plane plane = Plane::XY;
    std::vector<SketchEntity> entities;
};

}  // namespace graph3d::geometry
