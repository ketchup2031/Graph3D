#include "drawing/ModelReference.h"
#include "drawing/DraftCurves.h"

#include <QLineF>
#include <QRectF>

#include <algorithm>

namespace graph3d::drawing {
namespace {

template<class Visitor>
void visitViews(const ModelViews& model, Visitor visit)
{
    if (!model.geometry) {
        return;
    }
    const auto& [front, top, right] = model.geometry->views;
    const std::array views{&front, &right, &top};
    const auto& section = model.geometry->section;
    constexpr std::array planes{geometry::Plane::XZ, geometry::Plane::YZ, geometry::Plane::XY};
    for (std::size_t i = 0; i < views.size(); ++i) {
        if (!model.showBase[i]) {
            continue;
        }
        const bool cut = section && section->plane == planes[i];
        if (!cut || model.local) {
            visit(*views[i], model.layout[i], std::nullopt);
        }
        if (cut) {
            visit(section->view, model.layout[i], std::nullopt);
        }
    }
    for (const auto& extra : model.additional) {
        if (!extra.geometry) {
            continue;
        }
        auto placement = extra.placement;
        std::optional<QRectF> window;
        if (extra.kind == DrawingViewKind::Detail) {
            const auto radius = extra.radius * placement.scale;
            window = QRectF(placement.center - QPointF(radius, radius), QSizeF(2 * radius, 2 * radius));
            placement.center -= QPointF(extra.focus.x(), -extra.focus.y()) * placement.scale;
        } else if (extra.kind == DrawingViewKind::Local) {
            const auto parent = locateModelView(model, extra.source);
            if (!parent) {
                continue;
            }
            placement = parent->placement;
            const auto center = placement.center + QPointF(extra.focus.x(), -extra.focus.y()) * placement.scale;
            const auto radius = extra.radius * placement.scale;
            window = QRectF(center - QPointF(radius, radius), QSizeF(2 * radius, 2 * radius));
        }
        visit(extra.geometry->view, placement, window);
    }
}

bool inside(QPointF point, const std::optional<QRectF>& window)
{
    return !window || QLineF(point, window->center()).length() <= window->width() / 2;
}

DraftCurve onPaper(const ProjectedEdge& edge, const ViewPlacement& place)
{
    const auto point = [&](Point p) { return place.center + QPointF(p.x, -p.y) * place.scale; };
    return {point(edge.a), point(edge.b), edge.radius * place.scale, -edge.angle, -edge.sweep};
}

}  // namespace

bool isModelReference(const DraftReference& reference)
{
    return reference.anchor == DraftAnchor::ModelCurve || reference.anchor == DraftAnchor::ModelCenter;
}

std::optional<ModelPick> pickModelGeometry(const ModelViews& model, QPointF page,
                                           double tolerance, bool circularOnly)
{
    std::optional<ModelPick> result;
    visitViews(model, [&](const View& view, const ViewPlacement& place, const std::optional<QRectF>& window) {
        for (const auto& edge : view.edges) {
            if (circularOnly && edge.radius <= 0) {
                continue;
            }
            const auto curve = onPaper(edge, place);
            const auto consider = [&](QPointF point, double parameter, DraftAnchor anchor) {
                const double distance = QLineF(point, page).length();
                if (distance < tolerance && inside(point, window)) {
                    tolerance = distance;
                    result = ModelPick{point, {edge.id, anchor, parameter, {}, view.id}};
                }
            };
            if (!circularOnly && edge.radius > 0) {
                consider(curve.a, 0, DraftAnchor::ModelCenter);
            }
            const double parameter = curve.nearest(page);
            consider(curve.at(parameter), parameter, DraftAnchor::ModelCurve);
        }
    });
    return result;
}

std::optional<QPointF> modelReferencePoint(const ModelViews& model, const DraftReference& reference)
{
    std::optional<QPointF> result;
    visitViews(model, [&](const View& view, const ViewPlacement& place, const std::optional<QRectF>& window) {
        if (view.id != reference.view) {
            return;
        }
        const auto edge = std::ranges::find(view.edges, reference.item, &ProjectedEdge::id);
        if (edge == view.edges.end()) {
            return;
        }
        const auto curve = onPaper(*edge, place);
        if (reference.anchor == DraftAnchor::ModelCenter) {
            if (curve.radius > 0) {
                result = curve.a;
            }
        } else if (reference.parameter >= 0 && reference.parameter <= 1 && inside(curve.at(reference.parameter), window)) {
            result = curve.at(reference.parameter);
        }
    });
    return result;
}

std::optional<double> modelReferenceScale(const ModelViews& model, const DraftReference& reference)
{
    std::optional<double> scale;
    visitViews(model, [&](const View& view, const ViewPlacement& place, const std::optional<QRectF>&) {
        if (view.id == reference.view) {
            scale = place.scale;
        }
    });
    return scale;
}

}  // namespace graph3d::drawing
