#pragma once

#include "geometry/PartFeatures.h"

#include <span>
#include <vector>
#include <string>
#include <gp_Trsf.hxx>
#include <TopoDS_Shape.hxx>

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

enum class MateKind { Plane, Cylinder };

struct MateSurface {
    MateKind kind = MateKind::Plane;
    Point3 point{};
    Point3 direction{};
    double radius = 0;
    bool operator==(const MateSurface&) const = default;
};

struct Mate {
    int component = 0;
    int target = 0;
    MateSurface moving;
    MateSurface reference;
    bool opposite = true;
    bool operator==(const Mate&) const = default;
};

struct AssemblyComponentState {
    gp_Trsf transform;
    int translations = 3;
    int rotations = 3;
};

struct AssemblySolution {
    std::vector<AssemblyComponentState> components;
    int failedMate = -1;
    std::string error;
};

struct Interference {
    std::vector<int> first;
    std::vector<int> second;
    double volumeMm3;
};

struct PlacedBody {
    const TopoDS_Shape& shape;
    Placement placement;
    bool fixed = false;
};

gp_Trsf assemblyPlacement(const Placement& placement);
std::vector<MateSurface> assemblySurfaces(const TopoDS_Shape& shape);
AssemblySolution solveAssembly(std::span<const PlacedBody> bodies, std::span<const Mate> mates = {});
TopoDS_Shape makeAssembly(std::span<const PlacedBody> bodies, std::span<const Mate> mates = {});
std::vector<Interference> checkInterference(const TopoDS_Shape& assembly);

}  // namespace graph3d::geometry
