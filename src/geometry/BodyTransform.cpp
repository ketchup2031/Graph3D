#include "geometry/BodyTransform.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <gp.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <type_traits>

namespace graph3d::geometry {
namespace {

gp_Dir direction(ModelAxis axis)
{
    switch (axis) {
    case ModelAxis::X: return gp::DX();
    case ModelAxis::Y: return gp::DY();
    case ModelAxis::Z: return gp::DZ();
    }
    throw std::invalid_argument("Select a model axis.");
}

gp_Dir normal(Plane plane)
{
    switch (plane) {
    case Plane::XY: return gp::DZ();
    case Plane::XZ: return gp::DY();
    case Plane::YZ: return gp::DX();
    }
    throw std::invalid_argument("Select a mirror plane.");
}

template<class TransformAt>
TopoDS_Shape makeArray(const TopoDS_Shape& body, int count, TransformAt transformAt)
{
    if (count < 2 || count > maxArrayInstances) {
        throw std::invalid_argument("An array needs 2 to 100 instances.");
    }
    constexpr int maxSolids = 1000;
    int solids = 0;
    for (TopExp_Explorer explorer(body, TopAbs_SOLID); explorer.More(); explorer.Next()) {
        if (++solids > maxSolids / count) {
            throw std::invalid_argument("An array can contain at most 1000 solids.");
        }
    }

    BRep_Builder builder;
    TopoDS_Compound array;
    builder.MakeCompound(array);
    builder.Add(array, body);
    for (int i = 1; i < count; ++i) {
        // Rigid copies can share topology and carry their own locations.
        builder.Add(array, BRepBuilderAPI_Transform(body, transformAt(i)).Shape());
    }
    return array;
}

}  // namespace

TopoDS_Shape transformBody(const TopoDS_Shape& body, const BodyTransform& parameters)
{
    if (body.IsNull() || !TopExp_Explorer(body, TopAbs_SOLID).More()) {
        throw std::invalid_argument("Select a solid body.");
    }
    return std::visit([&body](const auto& operation) -> TopoDS_Shape {
        using Type = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Type, LinearArray>) {
            if (!std::isfinite(operation.spacing) || std::abs(operation.spacing) <= Precision::Confusion()
                || !std::isfinite(operation.spacing * operation.count)) {
                throw std::invalid_argument("Enter a finite, nonzero spacing.");
            }
            const gp_Vec step = gp_Vec(direction(operation.axis)) * operation.spacing;
            return makeArray(body, operation.count, [&step](int i) {
                gp_Trsf move;
                move.SetTranslation(step * i);
                return move;
            });
        } else if constexpr (std::is_same_v<Type, PolarArray>) {
            if (!std::isfinite(operation.sweep) || operation.sweep <= 0 || operation.sweep > 360) {
                throw std::invalid_argument("Enter a sweep greater than 0 and up to 360 degrees.");
            }
            const gp_Ax1 axis(gp::Origin(), direction(operation.axis));
            return makeArray(body, operation.count, [&](int i) {
                const int intervals = operation.sweep == 360 ? operation.count : operation.count - 1;
                gp_Trsf rotate;
                rotate.SetRotation(axis, i * (operation.sweep / intervals) * std::numbers::pi / 180);
                return rotate;
            });
        } else {
            if (!std::isfinite(2 * operation.offset)) {
                throw std::invalid_argument("Enter a finite mirror plane offset.");
            }
            const auto perpendicular = normal(operation.plane);
            const auto origin = gp::Origin().Translated(gp_Vec(perpendicular) * operation.offset);
            gp_Trsf reflect;
            reflect.SetMirror(gp_Ax2(origin, perpendicular));
            return BRepBuilderAPI_Transform(body, reflect).Shape();
        }
    }, parameters);
}

}  // namespace graph3d::geometry
