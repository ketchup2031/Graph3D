#pragma once

#include "geometry/Sketch.h"

#include <variant>

class TopoDS_Shape;

namespace graph3d::geometry {

constexpr int maxArrayInstances = 100;
enum class ModelAxis { X, Y, Z };

struct LinearArray {
    ModelAxis axis;
    int count;
    double spacing;
    bool operator==(const LinearArray&) const = default;
};

struct PolarArray {
    ModelAxis axis;
    int count;
    double sweep;
    bool operator==(const PolarArray&) const = default;
};

struct Mirror {
    Plane plane;
    double offset;
    bool operator==(const Mirror&) const = default;
};

using BodyTransform = std::variant<LinearArray, PolarArray, Mirror>;

TopoDS_Shape transformBody(const TopoDS_Shape& body, const BodyTransform& parameters);

}  // namespace graph3d::geometry
