#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

class TopoDS_Face;
class TopoDS_Edge;
class TopoDS_Shape;
class TopoDS_Wire;

namespace graph3d::geometry {

using Point3 = std::array<double, 3>;

struct FaceReference { Point3 center; Point3 outward; bool operator==(const FaceReference&) const = default; };
struct EdgeReference { std::array<Point3, 5> samples; int curveType; bool operator==(const EdgeReference&) const = default; };
enum class HoleStyle { Straight, Counterbore, Countersink };
struct ThreadSize {
    double diameter, pitch, length;
    bool leftHanded = false;
    bool operator==(const ThreadSize&) const = default;
};
struct HoleSize {
    double diameter; double depth; bool through;
    HoleStyle style = HoleStyle::Straight;
    double headDiameter = 0, seat = 0; // Counterbore depth or countersink included angle.
    std::optional<ThreadSize> thread;
    bool operator==(const HoleSize&) const = default;
};
struct BlendReference {
    Point3 center;
    double area;
    int surface;
    bool operator==(const BlendReference&) const = default;
};
enum class EdgeTreatmentKind { Chamfer, Fillet };

FaceReference locateHole(const TopoDS_Face& face, Point3 rayOrigin, Point3 rayDirection, bool sketch = false);
EdgeReference referenceEdge(const TopoDS_Edge& edge);
std::vector<FaceReference> planarFaces(const TopoDS_Shape& body);
std::vector<EdgeReference> twoFaceEdges(const TopoDS_Shape& body);
TopoDS_Face resolveExactPlanarFace(const TopoDS_Shape& body, const FaceReference& reference);
TopoDS_Shape treatEdges(const TopoDS_Shape& body, std::span<const EdgeReference> edges, EdgeTreatmentKind kind,
                       double size, bool followTangency = false, std::optional<double> endRadius = {});
TopoDS_Face resolveSketchFace(const TopoDS_Shape& body, const FaceReference& reference);

TopoDS_Shape drillHole(const TopoDS_Shape& body, const FaceReference& face, HoleSize size);
TopoDS_Shape treatEdge(const TopoDS_Shape& body, const EdgeReference& edge, EdgeTreatmentKind kind, double size);
BlendReference referenceBlend(const TopoDS_Face& face);
std::vector<BlendReference> blendFaces(const TopoDS_Shape& body);
TopoDS_Shape removeBlend(const TopoDS_Shape& body, const BlendReference& reference);
TopoDS_Shape movePlanarFace(const TopoDS_Shape& body, const FaceReference& reference, double distance);
std::vector<Point3> pathArrayOffsets(const TopoDS_Wire& path, int count, bool reverse);
TopoDS_Shape patternFeature(const TopoDS_Shape& base, const TopoDS_Shape& seed, std::span<const Point3> offsets);

}  // namespace graph3d::geometry
