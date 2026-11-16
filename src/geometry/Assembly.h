#pragma once

#include "geometry/PartFeatures.h"

#include <span>

namespace graph3d::geometry {

constexpr int minAssemblyComponents = 2;
constexpr int maxAssemblyComponents = 100;
constexpr double maxComponentOffset = 1'000'000;
constexpr double maxComponentAngle = 360;

struct Placement {
    int anchor = 0;
    Point3 offset{};
    Point3 angles{};
    bool operator==(const Placement&) const = default;
};

struct PlacedBody {
    const TopoDS_Shape& shape;
    Placement placement;
};

TopoDS_Shape makeAssembly(std::span<const PlacedBody> bodies);

}  // namespace graph3d::geometry
