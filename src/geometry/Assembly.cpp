#include "geometry/Assembly.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <gp.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace graph3d::geometry {
namespace {

gp_Trsf localPlacement(const Placement& placement)
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

}  // namespace

TopoDS_Shape makeAssembly(std::span<const PlacedBody> bodies)
{
    if (bodies.size() < minAssemblyComponents || bodies.size() > maxAssemblyComponents) {
        throw std::invalid_argument("An assembly needs 2 to 100 components.");
    }
    BRep_Builder builder;
    TopoDS_Compound assembly;
    builder.MakeCompound(assembly);
    std::vector<gp_Trsf> placements;
    placements.reserve(bodies.size() + 1);
    placements.emplace_back(); // Assembly origin, referenced as anchor 0.
    int totalSolids = 0;
    for (const auto& body : bodies) {
        const int anchor = body.placement.anchor;
        if (anchor < 0 || static_cast<std::size_t>(anchor) >= placements.size()) {
            throw std::invalid_argument("An anchor must be the origin or an earlier component.");
        }
        if (body.shape.IsNull() || !TopExp_Explorer(body.shape, TopAbs_SOLID).More()) {
            throw std::invalid_argument("Each component needs a solid body.");
        }
        for (TopExp_Explorer explorer(body.shape, TopAbs_SOLID); explorer.More(); explorer.Next()) {
            if (++totalSolids > 1000) {
                throw std::invalid_argument("An assembly can contain at most 1000 solids.");
            }
        }
        placements.push_back(placements[anchor] * localPlacement(body.placement));
        // Copy geometry so coincident instances remain separate solids during export.
        builder.Add(assembly, BRepBuilderAPI_Transform(body.shape, placements.back(), true).Shape());
    }
    return assembly;
}

}  // namespace graph3d::geometry
