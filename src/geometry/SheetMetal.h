#pragma once

#include <TopoDS_Shape.hxx>
#include <vector>

namespace graph3d::geometry {

struct SheetBend {
    bool left = false;
    double flange = 20, radius = 2, degrees = 90, factor = .42, relief = 0;
    bool operator==(const SheetBend&) const = default;
};
struct SheetHole {
    int panel = 0; // 0 is base; bend index + 1 is its flange.
    double along = 10, across = 10, diameter = 6;
    bool operator==(const SheetHole&) const = default;
};
struct SheetMetal {
    double length = 60, width = 40, thickness = 1.5;
    std::vector<SheetBend> bends;
    std::vector<SheetHole> holes;
    bool operator==(const SheetMetal&) const = default;
};
struct FlatHole { double x, y, radius; };
struct FlatBend { double begin, end, degrees, relief; };
struct FlatSheet {
    double length, width;
    std::vector<FlatBend> bends;
    std::vector<FlatHole> holes;
};
FlatSheet flattenSheet(const SheetMetal& sheet);
TopoDS_Shape makeSheetMetal(const SheetMetal& sheet);
}  // namespace graph3d::geometry
