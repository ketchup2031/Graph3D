#include "geometry/Assembly.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <gp.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <array>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace graph3d::geometry {

gp_Trsf assemblyPlacement(const Placement& placement)
{
    gp_Trsf transform;
    for (const auto value : placement.offset) {
        if (!std::isfinite(value) || std::abs(value) > maxComponentOffset) {
            throw std::invalid_argument("The component offset must be within ±1,000,000 mm.");
        }
    }
    transform.SetTranslation(gp_Vec(placement.offset[0], placement.offset[1], placement.offset[2]));
    const std::array axes{gp::OZ(), gp::OY(), gp::OX()};
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const double angle = placement.angles[2 - i];
        if (!std::isfinite(angle) || std::abs(angle) > maxComponentAngle) {
            throw std::invalid_argument("Each component angle must be within ±360 degrees.");
        }
        if (angle != 0) {
            gp_Trsf rotation;
            rotation.SetRotation(axes[i], angle * std::numbers::pi / 180);
            transform.Multiply(rotation);
        }
    }
    return transform;
}

TopoDS_Shape makeAssembly(std::span<const PlacedBody> bodies, std::span<const Mate> mates, double driveAngle, TopoDS_Shape* preview)
{
    if (bodies.empty() || bodies.size() > maxAssemblyComponents) {
        throw std::invalid_argument("An assembly needs 1 to 100 components.");
    }
    BRep_Builder builder;
    TopoDS_Compound assembly;
    builder.MakeCompound(assembly);
    TopoDS_Compound simplified;
    const bool simplify = preview && std::ranges::any_of(bodies, [](const auto& body) { return body.simplified; });
    if (simplify) { builder.MakeCompound(simplified); }
    const auto solution = solveAssembly(bodies, mates, driveAngle);
    if (!solution.error.empty()) {
        const auto prefix = solution.failedMate < 0 ? std::string{} : "Mate #" + std::to_string(solution.failedMate + 1) + ": ";
        throw std::invalid_argument(prefix + solution.error);
    }
    int totalSolids = 0;
    for (std::size_t index = 0; index < bodies.size(); ++index) {
        const auto& body = bodies[index];
        const auto& full = solution.contextualShapes.empty() ? body.shape : solution.contextualShapes[index];
        if (full.IsNull() || !TopExp_Explorer(full, TopAbs_SOLID).More()) {
            throw std::invalid_argument("Each component needs a solid body.");
        }
        for (TopExp_Explorer explorer(full, TopAbs_SOLID); explorer.More(); explorer.Next()) {
            if (++totalSolids > 1000) {
                throw std::invalid_argument("An assembly can contain at most 1000 solids.");
            }
        }
        // Copy geometry so coincident instances remain separate solids during export.
        const auto transformed = BRepBuilderAPI_Transform(full, solution.components[index].transform, true).Shape();
        builder.Add(assembly, transformed);
        if (simplify && body.simplified) {
            Bnd_Box bounds;
            BRepBndLib::AddOptimal(full, bounds, false, false);
            const auto box = BRepPrimAPI_MakeBox(bounds.CornerMin(), bounds.CornerMax()).Shape();
            builder.Add(simplified, BRepBuilderAPI_Transform(box, solution.components[index].transform, true).Shape());
        } else if (simplify) {
            builder.Add(simplified, transformed);
        }
    }
    if (preview) { *preview = simplified; }
    return assembly;
}

}  // namespace graph3d::geometry
