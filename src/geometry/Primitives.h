#pragma once

#include "geometry/Primitive.h"

#include <TopoDS_Shape.hxx>

namespace graph3d::geometry {

// All primitives rest on Z=0 and fit within the same 100 mm square footprint.
TopoDS_Shape makePrimitive(Primitive primitive, double x = 0.0, double y = 0.0);

}  // namespace graph3d::geometry
