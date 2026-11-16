#pragma once

#include "geometry/Sketch.h"

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

namespace graph3d::geometry {

TopoDS_Face makeProfile(const Sketch& sketch);
TopoDS_Shape extrude(const TopoDS_Face& face, Plane plane, double distance);
TopoDS_Shape revolve(const TopoDS_Face& face, Plane plane, Axis axis, double offset, double degrees);
TopoDS_Shape booleanResult(const TopoDS_Shape& first, const TopoDS_Shape& second, BooleanOperation operation);

}  // namespace graph3d::geometry
