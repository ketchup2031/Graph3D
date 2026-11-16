#pragma once

#include "geometry/Sketch.h"

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>

namespace graph3d::geometry {

struct SketchContext {
    gp_Ax3 frame;
    std::vector<SketchProjection> projections;
};

SketchContext sketchContext(const Sketch& sketch, const TopoDS_Shape& support = {});
TopoDS_Face makeProfile(const Sketch& sketch);
TopoDS_Face makeProfile(const Sketch& sketch, const SketchContext& context);
TopoDS_Shape extrude(const TopoDS_Face& face, const gp_Ax3& frame, double distance);
TopoDS_Shape revolve(const TopoDS_Face& face, const gp_Ax3& frame, Axis axis, double offset, double degrees);
TopoDS_Shape extrude(const TopoDS_Face& face, Plane plane, double distance);
TopoDS_Shape revolve(const TopoDS_Face& face, Plane plane, Axis axis, double offset, double degrees);
TopoDS_Shape booleanResult(const TopoDS_Shape& first, const TopoDS_Shape& second, BooleanOperation operation);

}  // namespace graph3d::geometry
