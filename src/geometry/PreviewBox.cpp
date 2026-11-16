#include "geometry/PreviewBox.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>

#include <stdexcept>

namespace mechforge::geometry {
namespace {

Point pointOf(const TopoDS_Vertex& vertex)
{
    const gp_Pnt p = BRep_Tool::Pnt(vertex);
    return {p.X(), p.Y(), p.Z()};
}

}  // namespace

std::vector<Segment> makePreviewBox()
{
    try {
        const TopoDS_Shape shape = BRepPrimAPI_MakeBox(100.0, 100.0, 100.0).Shape();
        if (shape.IsNull() || !BRepCheck_Analyzer(shape).IsValid()) {
            throw std::runtime_error("OCCT did not produce a valid test solid.");
        }

        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(shape, TopAbs_EDGE, edges);
        std::vector<Segment> segments;
        for (int i = 1; i <= edges.Extent(); ++i) {
            TopoDS_Vertex start;
            TopoDS_Vertex end;
            TopExp::Vertices(TopoDS::Edge(edges(i)), start, end);
            segments.push_back({pointOf(start), pointOf(end)});
        }
        return segments;
    } catch (const Standard_Failure& failure) {
        const char* message = failure.GetMessageString();
        throw std::runtime_error(message ? message : "OCCT could not build the test solid.");
    }
}

}  // namespace mechforge::geometry
