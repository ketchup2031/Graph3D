#pragma once

#include "geometry/PartFeatures.h"

#include <TopoDS_Shape.hxx>
#include <span>

namespace graph3d::geometry {

struct SpatialCurve {
    std::vector<Point3> points;
    bool spline = false;
    bool closed = false;
    bool operator==(const SpatialCurve&) const = default;
};

enum class SurfaceKind { Loft, Sweep, Network, Trim, Extend, Patch, Sew, Solid, OpenEdges };

TopoDS_Shape makeSpatialCurve(const SpatialCurve& curve);
TopoDS_Shape buildSurface(SurfaceKind kind, std::span<const TopoDS_Shape> shapes,
    double distance, int index);

}  // namespace graph3d::geometry
