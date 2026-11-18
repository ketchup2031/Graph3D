#pragma once

#include "geometry/PartFeatures.h"

#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>

#include <span>

namespace graph3d::geometry {

struct DatumPlane {
    Point3 origin{0, 0, 0};
    Point3 normal{0, 0, 1};
    bool operator==(const DatumPlane&) const = default;
};
struct DatumAxis {
    Point3 origin{0, 0, 0};
    Point3 direction{0, 0, 1};
    bool operator==(const DatumAxis&) const = default;
};
struct MassProperties {
    double volumeMm3;
    Point3 centerMm;
    double massKg;
};

gp_Ax3 datumFrame(const DatumPlane& plane);
gp_Ax3 rotationFrame(const DatumAxis& axis, const gp_Ax3& profile);
TopoDS_Shape datumOutline(const DatumPlane& plane);
TopoDS_Shape datumOutline(const DatumAxis& axis);
TopoDS_Shape sweepProfile(const TopoDS_Face& profile, const gp_Ax3& frame, std::span<const Point3> path);
TopoDS_Shape sweepProfile(const TopoDS_Face& profile, const gp_Ax3& frame, const TopoDS_Wire& path, bool reverse);
TopoDS_Shape loftProfiles(std::span<const TopoDS_Face> profiles, bool ruled);
TopoDS_Shape shellBody(const TopoDS_Shape& body, std::span<const FaceReference> openings, double thickness);
TopoDS_Shape draftBody(const TopoDS_Shape& body, const FaceReference& neutral,
                       std::span<const FaceReference> walls, double degrees);
MassProperties massProperties(const TopoDS_Shape& body, double densityKgM3);
double minimumDistance(const TopoDS_Shape& first, const TopoDS_Shape& second);

}  // namespace graph3d::geometry
