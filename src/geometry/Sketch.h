#pragma once

#include "geometry/PartFeatures.h"

#include <optional>
#include <variant>
#include <vector>

namespace graph3d::geometry {

enum class Plane { XY, XZ, YZ };
enum class Axis { U, V };
enum class BooleanOperation { Union, Difference, Intersection };

struct Point2 {
    double u;
    double v;
    bool operator==(const Point2&) const = default;
};

struct Line { Point2 start, end; bool operator==(const Line&) const = default; };
struct Rectangle { Point2 first, opposite; bool operator==(const Rectangle&) const = default; };
struct Circle { Point2 center; double radius; bool operator==(const Circle&) const = default; };
using SketchEntity = std::variant<Line, Rectangle, Circle>;

// A vertex or edge of an entity, indexed separately from its position in the document.
struct SketchRef {
    int entity = 0;
    int part = 0;
    bool operator==(const SketchRef&) const = default;
};

enum class ConstraintKind { Coincident, Horizontal, Vertical, Perpendicular, Length, Diameter, OnProjection };

struct SketchConstraint {
    ConstraintKind kind = ConstraintKind::Coincident;
    SketchRef first;
    SketchRef second;
    double value = 0;
    std::optional<EdgeReference> edge;
    bool operator==(const SketchConstraint&) const = default;
};

struct SketchSupport {
    int body;
    FaceReference face;
    bool operator==(const SketchSupport&) const = default;
};

struct SketchProjection {
    EdgeReference edge;
    std::vector<Point2> points;
};

struct Sketch {
    Plane plane = Plane::XY;
    std::vector<SketchEntity> entities;
    std::vector<SketchConstraint> constraints;
    std::optional<SketchSupport> support;
    bool operator==(const Sketch&) const = default;
};

}  // namespace graph3d::geometry
