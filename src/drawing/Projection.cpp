#include "drawing/Projection.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepLib.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace graph3d::drawing {
namespace {

void appendEdges(Linework& lines, const TopoDS_Shape& edges, double tolerance)
{
    if (edges.IsNull()) {
        return;
    }
    if (!BRepLib::BuildCurves3d(edges)) {
        throw std::runtime_error("Could not build the curves of the projection.");
    }
    for (TopExp_Explorer it(edges, TopAbs_EDGE); it.More(); it.Next()) {
        const BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));
        const auto first = curve.FirstParameter();
        const auto last = curve.LastParameter();
        std::vector<Point> points;
        const auto append = [&points](const gp_Pnt& vertex) {
            if (points.empty() || points.back().x != vertex.X() || points.back().y != vertex.Y()) {
                points.push_back({vertex.X(), vertex.Y()});
            }
        };
        if (curve.GetType() == GeomAbs_Line) {
            append(curve.Value(first));
            append(curve.Value(last));
        } else {
            GCPnts_QuasiUniformDeflection samples(curve, tolerance, first, last);
            if (!samples.IsDone()) {
                throw std::runtime_error("Could not sample a curved projection edge.");
            }
            points.reserve(static_cast<std::size_t>(samples.NbPoints()));
            for (int i = 1; i <= samples.NbPoints(); ++i) {
                append(samples.Value(i));
            }
        }
        if (points.size() > 1) {
            lines.push_back(std::move(points));
        }
    }
}

View makeView(const TopoDS_Shape& solid, const gp_Ax2& placement, double width, double height)
{
    const Handle(HLRBRep_Algo) algorithm = new HLRBRep_Algo;
    algorithm->Add(solid, 0);
    algorithm->Projector(HLRAlgo_Projector(placement));
    algorithm->Update();
    algorithm->Hide();

    HLRBRep_HLRToShape edges(algorithm);
    View view{{}, {}, width, height};
    // Approximate only the display of curves, to less than 0.02 mm on the A4 drawing.
    const double tolerance = std::max(width, height) * 0.00015;
    appendEdges(view.visible, edges.VCompound(), tolerance);
    appendEdges(view.visible, edges.OutLineVCompound(), tolerance);
    appendEdges(view.hidden, edges.HCompound(), tolerance);
    appendEdges(view.hidden, edges.OutLineHCompound(), tolerance);
    if (view.visible.empty()) {
        throw std::runtime_error("No visible edges were found for an orthographic view.");
    }
    return view;
}

}  // namespace

Projections project(const TopoDS_Shape& solid)
{
    try {
        if (solid.IsNull() || !TopExp_Explorer(solid, TopAbs_SOLID).More()) {
            throw std::invalid_argument("Select a solid body to create a drawing.");
        }
        Bnd_Box bounds;
        BRepBndLib::AddOptimal(solid, bounds, false, false);
        if (bounds.IsVoid() || bounds.IsOpen()) {
            throw std::invalid_argument("The body has no finite drawing bounds.");
        }
        const auto low = bounds.CornerMin();
        const auto high = bounds.CornerMax();
        const double x = high.X() - low.X();
        const double y = high.Y() - low.Y();
        const double z = high.Z() - low.Z();
        if (!(x > 0 && y > 0 && z > 0) || !std::isfinite(std::max({x, y, z}))) {
            throw std::invalid_argument("The body has invalid dimensions.");
        }
        const gp_Pnt center((low.X() + high.X()) / 2, (low.Y() + high.Y()) / 2,
                            (low.Z() + high.Z()) / 2);
        return {
            makeView(solid, gp_Ax2(center, gp_Dir(0, -1, 0), gp_Dir(1, 0, 0)), x, z),
            makeView(solid, gp_Ax2(center, gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)), x, y),
            makeView(solid, gp_Ax2(center, gp_Dir(1, 0, 0), gp_Dir(0, 1, 0)), y, z)
        };
    } catch (const Standard_Failure& failure) {
        const auto* message = failure.GetMessageString();
        throw std::runtime_error(message && *message ? message : "Open CASCADE could not compute the projections.");
    }
}

}  // namespace graph3d::drawing
