#pragma once

#include "geometry/Sketch.h"

#include <array>
#include <optional>

namespace graph3d::geometry {

struct ArcGeometry {
    Point2 center;
    double radius;
    double start;
    double sweep;
};

std::optional<ArcGeometry> sketchArc(const Arc& arc);
std::vector<std::array<Point2, 4>> sketchSplineSpans(const Spline& spline);

}  // namespace graph3d::geometry
