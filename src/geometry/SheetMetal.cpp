#include "geometry/SheetMetal.h"
#include "geometry/SketchModeling.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace graph3d::geometry {
namespace {
constexpr double eps = 1.e-5;
constexpr double radians = std::numbers::pi / 180;
struct StripPoint {
    double x, z;
    StripPoint operator+(StripPoint b) const { return {x+b.x, z+b.z}; }
    StripPoint operator*(double s) const { return {x*s, z*s}; }
    gp_Pnt at(double y) const { return {x,y,z}; }
};
struct Panel {
    StripPoint origin, tangent, normal;
    double length, flat;
    bool left;
};
struct Fold {
    StripPoint center;
    double start, angle, signedBottomRadius, signedTopRadius;
    double relief;
    bool left;
    StripPoint point(double at, bool top) const {
        const double phi = start + angle * at;
        const double radius = top ? signedTopRadius : signedBottomRadius;
        return center + StripPoint{-std::sin(phi)*(left ? -1 : 1), std::cos(phi)} * radius;
    }
};
struct Layout {
    std::vector<Panel> panels;
    std::vector<Fold> folds;
    FlatSheet flat;
};

void size(double value)
{
    if (!std::isfinite(value) || value < .01 || value > 100000) { throw std::invalid_argument("Sheet dimensions must be between 0.01 and 100,000 mm."); }
}

Layout layout(const SheetMetal& sheet)
{
    size(sheet.length); size(sheet.width); size(sheet.thickness);
    if (sheet.bends.size() > 24 || sheet.holes.size() > 128) { throw std::invalid_argument("Too many sheet bends or holes."); }
    Layout result{{{{0, 0}, {1, 0}, {0, 1}, sheet.length, 0, false}}, {}, {sheet.length, sheet.width}};
    std::array<StripPoint, 2> ends{StripPoint{0,0}, StripPoint{sheet.length,0}};
    std::array<double, 2> angles{}, flat{0, sheet.length};
    for (const auto& bend : sheet.bends) {
        size(bend.flange); size(bend.radius);
        if (!std::isfinite(bend.degrees) || std::abs(bend.degrees) < 1 || std::abs(bend.degrees) >= 180
            || !std::isfinite(bend.factor) || bend.factor < 0 || bend.factor > 1
            || !std::isfinite(bend.relief) || bend.relief < 0 || bend.relief >= sheet.width/2 - eps) {
            throw std::invalid_argument("Use a bend angle from 1 to under 180 degrees, a K factor from 0 to 1, and relief smaller than half the width.");
        }
        const int side = bend.left ? 0 : 1;
        const double sign = bend.left ? -1 : 1;
        const double start = angles[side];
        const double turn = bend.degrees * radians;
        const double into = turn > 0 ? bend.radius + sheet.thickness : -bend.radius;
        const StripPoint normal{-sign*std::sin(start), std::cos(start)};
        const double allowance = std::abs(turn) * (bend.radius + bend.factor*sheet.thickness);
        Fold fold{ends[side] + normal*into, start, turn, -into, sheet.thickness-into, bend.relief, bend.left};
        result.folds.push_back(fold);
        const auto begin = flat[side];
        flat[side] += sign*allowance;
        result.flat.bends.push_back({std::min(begin,flat[side]), std::max(begin,flat[side]), bend.degrees, bend.relief});
        angles[side] += turn;
        const StripPoint tangent{sign*std::cos(angles[side]), std::sin(angles[side])};
        const StripPoint newNormal{-sign*std::sin(angles[side]), std::cos(angles[side])};
        const auto at = fold.point(1, false);
        result.panels.push_back({at,tangent,newNormal,bend.flange,flat[side],bend.left});
        ends[side] = at + tangent*bend.flange;
        flat[side] += sign*bend.flange;
    }
    result.flat.length = flat[1]-flat[0];
    if (result.flat.length > 1.e6) { throw std::invalid_argument("The developed sheet exceeds the model size limit."); }
    for (auto& bend : result.flat.bends) { bend.begin -= flat[0]; bend.end -= flat[0]; }
    for (const auto& hole : sheet.holes) {
        size(hole.diameter);
        if (hole.panel < 0 || hole.panel >= static_cast<int>(result.panels.size())) {
            throw std::invalid_argument("A sheet hole refers to a missing panel.");
        }
        const auto& p = result.panels[hole.panel];
        const double r = hole.diameter/2;
        if (!std::isfinite(hole.along) || !std::isfinite(hole.across) || hole.along <= r+eps || hole.across <= r+eps
            || hole.along >= p.length-r-eps || hole.across >= sheet.width-r-eps) {
            throw std::invalid_argument("The hole must fit on the flat panel away from edges and bends.");
        }
        const double x = p.flat - flat[0] + (p.left ? -hole.along : hole.along);
        for (const auto& prior : result.flat.holes) {
            if (std::hypot(x-prior.x, hole.across-prior.y) <= r+prior.radius+eps) {
                throw std::invalid_argument("Holes intersect in the developed sheet.");
            }
        }
        result.flat.holes.push_back({x,hole.across,r});
    }
    return result;
}

TopoDS_Shape prism(BRepBuilderAPI_MakeWire& wire, double width)
{
    const auto face = BRepBuilderAPI_MakeFace(wire.Wire()).Face();
    auto result = BRepPrimAPI_MakePrism(face, gp_Vec(0,width,0)).Shape();
    GProp_GProps props;
    BRepGProp::VolumeProperties(result, props);
    if (props.Mass() < 0) { result.Reverse(); }
    return result;
}
void line(BRepBuilderAPI_MakeWire& wire, StripPoint a, StripPoint b, double y) {
    wire.Add(BRepBuilderAPI_MakeEdge(a.at(y),b.at(y)));
}
double volume(const TopoDS_Shape& shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape,props);
    return props.Mass();
}
TopoDS_Shape foldSheet(const SheetMetal& sheet, const Layout& plan)
{
    std::vector<TopoDS_Shape> segments;
    std::vector<Bnd_Box> bounds;
    const auto append = [&](TopoDS_Shape shape) {
        Bnd_Box box;
        BRepBndLib::AddOptimal(shape, box, false, true);
        for (std::size_t i = 0; i < segments.size(); ++i) {
            if (box.IsOut(bounds[i])) { continue; }
            BRepAlgoAPI_Common overlap(segments[i],shape);
            if (!overlap.IsDone() || (!overlap.Shape().IsNull() && volume(overlap.Shape()) > eps)) {
                throw std::invalid_argument("Folded sheet segments intersect.");
            }
        }
        bounds.push_back(box);
        segments.push_back(std::move(shape));
    };
    for (const auto& p : plan.panels) {
        const auto end = p.origin + p.tangent*p.length, top = p.normal*sheet.thickness;
        BRepBuilderAPI_MakeWire wire;
        line(wire,p.origin,end,0); line(wire,end,end+top,0);
        line(wire,end+top,p.origin+top,0); line(wire,p.origin+top,p.origin,0);
        append(prism(wire,sheet.width));
    }
    for (const auto& fold : plan.folds) {
        const double y = fold.relief;
        BRepBuilderAPI_MakeWire wire;
        const auto curve = [&](bool top, bool reverse) {
            const auto a = fold.point(reverse ? 1 : 0,top).at(y);
            const auto b = fold.point(.5,top).at(y);
            const auto c = fold.point(reverse ? 0 : 1,top).at(y);
            wire.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(a,b,c).Value()));
        };
        curve(false,false); line(wire,fold.point(1,false),fold.point(1,true),y);
        curve(true,true); line(wire,fold.point(0,true),fold.point(0,false),y);
        append(prism(wire,sheet.width-2*y));
    }
    // Iterate from base in bend order so each fused segment already touches the plate.
    auto result = segments.front();
    for (std::size_t i = 0; i < plan.folds.size(); ++i) {
        result = booleanResult(result,segments[plan.panels.size()+i],BooleanOperation::Union);
        result = booleanResult(result,segments[i+1],BooleanOperation::Union);
    }
    for (const auto& hole : sheet.holes) {
        const auto& panel = plan.panels[hole.panel];
        const auto at = panel.origin + panel.tangent*hole.along + panel.normal*(-eps);
        const auto drill = BRepPrimAPI_MakeCylinder(gp_Ax2(at.at(hole.across),gp_Dir(panel.normal.x,0,panel.normal.z)),
            hole.diameter/2,sheet.thickness+2*eps).Shape();
        result = booleanResult(result,drill,BooleanOperation::Difference);
    }
    TopExp_Explorer solids(result,TopAbs_SOLID);
    if (!solids.More() || !BRepCheck_Analyzer(result).IsValid()) { throw std::invalid_argument("The folded sheet is not a valid solid."); }
    const auto piece = solids.Current();
    solids.Next();
    if (solids.More()) { throw std::invalid_argument("The folded sheet is disconnected."); }
    return piece;
}
}  // namespace

TopoDS_Shape makeSheetMetal(const SheetMetal& sheet)
{
    return foldSheet(sheet, layout(sheet));
}

FlatSheet flattenSheet(const SheetMetal& sheet)
{
    auto plan = layout(sheet);
    foldSheet(sheet, plan);
    return std::move(plan.flat);
}
}  // namespace graph3d::geometry
