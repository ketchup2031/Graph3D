#pragma once

#include <vector>

namespace graph3d::geometry {

struct Point {
    double x;
    double y;
    double z;
};

struct Segment {
    Point start;
    Point end;
};

// Returns the straight edges of a valid 100 × 100 × 100 mm OCCT solid.
std::vector<Segment> makePreviewBox();

}  // namespace graph3d::geometry
