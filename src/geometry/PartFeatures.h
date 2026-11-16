#pragma once

#include <array>

class TopoDS_Face;
class TopoDS_Edge;
class TopoDS_Shape;

namespace graph3d::geometry {

using Point3 = std::array<double, 3>;

struct FaceReference { Point3 center; Point3 outward; };
struct EdgeReference { std::array<Point3, 5> samples; int curveType; };
struct HoleSize { double diameter; double depth; bool through; };
enum class EdgeTreatmentKind { Chamfer, Fillet };

FaceReference locateHole(const TopoDS_Face& face, Point3 rayOrigin, Point3 rayDirection);
EdgeReference referenceEdge(const TopoDS_Edge& edge);

TopoDS_Shape drillHole(const TopoDS_Shape& body, const FaceReference& face, HoleSize size);
TopoDS_Shape treatEdge(const TopoDS_Shape& body, const EdgeReference& edge, EdgeTreatmentKind kind, double size);

}  // namespace graph3d::geometry
