#include "geometry/Fastener.h"
#include "geometry/SketchModeling.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <gp_Ax2.hxx>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace graph3d::geometry {
namespace {
constexpr std::array l4{10, 12, 16, 20, 25, 30};
constexpr std::array l5{10, 12, 16, 20, 25, 30, 35};
constexpr std::array l6{12, 16, 20, 25, 30, 35, 40};
constexpr std::array l8{16, 20, 25, 30, 35, 40, 50, 60};
constexpr std::array l10{20, 25, 30, 35, 40, 50, 60, 70};
constexpr std::array l12{25, 30, 35, 40, 50, 60, 70, 80};
constexpr std::array<FastenerSize, 6> sizes{{
    {4, .7, 7, 2.8, 3.2, 4.3, 9, .8, l4},
    {5, .8, 8, 3.5, 4, 5.3, 10, 1, l5},
    {6, 1, 10, 4, 5, 6.4, 12, 1.6, l6},
    {8, 1.25, 13, 5.3, 6.5, 8.4, 16, 1.6, l8},
    {10, 1.5, 16, 6.4, 8, 10.5, 20, 2, l10},
    {12, 1.75, 18, 7.5, 10, 13, 24, 2.5, l12},
}};

TopoDS_Shape hex(double across, double height, double z = 0)
{
    BRepBuilderAPI_MakePolygon wire;
    const double radius = across / std::sqrt(3.);
    for (int i = 0; i < 6; ++i) {
        const double a = i * std::numbers::pi / 3;
        wire.Add(gp_Pnt(radius * std::cos(a), radius * std::sin(a), z));
    }
    wire.Close();
    const auto face = BRepBuilderAPI_MakeFace(wire.Wire()).Face();
    return BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, height)).Shape();
}
TopoDS_Shape round(double radius, double height, double z = 0)
{
    return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, z), gp_Dir(0, 0, 1)), radius, height).Shape();
}
}

std::span<const FastenerSize> fastenerCatalog() { return sizes; }

const FastenerSize& fastenerSize(Fastener item)
{
    const auto size = std::ranges::find(sizes, item.diameter, &FastenerSize::diameter);
    if (size == sizes.end()
        || (item.kind == FastenerKind::Bolt && std::ranges::find(size->lengths, item.length) == size->lengths.end())
        || ((item.kind == FastenerKind::Nut || item.kind == FastenerKind::Washer) && item.length != 0)
        || (item.kind != FastenerKind::Bolt && item.kind != FastenerKind::Nut && item.kind != FastenerKind::Washer)) {
        throw std::invalid_argument("This fastener size is unavailable in the catalog.");
    }
    return *size;
}

std::string fastenerDesignation(Fastener item)
{
    fastenerSize(item);
    const auto size = "M" + std::to_string(item.diameter);
    switch (item.kind) {
    case FastenerKind::Bolt: return "G3D-B " + size + "x" + std::to_string(item.length);
    case FastenerKind::Nut: return "G3D-N " + size;
    case FastenerKind::Washer: return "G3D-W " + size;
    }
    throw std::invalid_argument("Unknown fastener type.");
}

TopoDS_Shape makeFastener(Fastener item)
{
    const auto& size = fastenerSize(item);
    switch (item.kind) {
    case FastenerKind::Bolt:
        return booleanResult(round(size.diameter / 2., item.length + .1), hex(size.wrench, size.head, item.length),
            BooleanOperation::Union);
    case FastenerKind::Nut:
        return booleanResult(hex(size.wrench, size.nut), round(size.diameter / 2., size.nut + 2, -1), BooleanOperation::Difference);
    case FastenerKind::Washer:
        return booleanResult(round(size.washerOuter / 2, size.washerThickness),
            round(size.washerBore / 2, size.washerThickness + 2, -1), BooleanOperation::Difference);
    }
    throw std::invalid_argument("Unknown fastener type.");
}
}  // namespace graph3d::geometry
