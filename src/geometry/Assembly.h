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
enum class MateMode { Align, Distance, Angle, Travel, Screw };

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
    MateMode mode = MateMode::Align;
    double value = 0; // Distance (mm), angle (degrees) or signed screw lead (mm/turn).
    double minimum = 0;
    double maximum = 0;
    bool operator==(const Mate&) const = default;
};

struct ContextFaceEdit {
    int reference = 0;
    FaceReference face;        // The face to move, in the component's local coordinates.
    FaceReference targetFace;  // The plane to follow, in the reference component's local coordinates.
    double clearance = 0;      // Distance beyond the reference plane along the moving face normal.
    bool operator==(const ContextFaceEdit&) const = default;
};

struct AssemblyComponentState {
    gp_Trsf transform;
    int translations = 3;
    int rotations = 3;
    Point3 freeRotationAxis{};
};

struct AssemblySolution {
    std::vector<AssemblyComponentState> components;
    int failedMate = -1;
    std::string error;
    double driveAngle = 0;
    double screwTravel = 0;
    std::vector<TopoDS_Shape> contextualShapes;
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
    bool simplified = false;
    std::optional<ContextFaceEdit> context;
};

gp_Trsf assemblyPlacement(const Placement& placement);
std::vector<MateSurface> assemblySurfaces(const TopoDS_Shape& shape);
AssemblySolution solveAssembly(std::span<const PlacedBody> bodies, std::span<const Mate> mates = {}, double driveAngle = 0);
TopoDS_Shape makeAssembly(std::span<const PlacedBody> bodies, std::span<const Mate> mates = {},
                         double driveAngle = 0, TopoDS_Shape* preview = nullptr);
std::vector<Interference> checkInterference(const TopoDS_Shape& assembly);

}  // namespace graph3d::geometry
