#include "drawing/ModelViews.h"

#include <QCoreApplication>
#include <QHash>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace graph3d::drawing {

bool derivedView(DrawingViewKind kind)
{
    return kind >= DrawingViewKind::Projected;
}

bool pathView(DrawingViewKind kind)
{
    return kind == DrawingViewKind::Projected || kind == DrawingViewKind::Arrow
        || kind == DrawingViewKind::SteppedSection || kind == DrawingViewKind::AlignedSection;
}

QString drawingViewName(DrawingViewKind kind)
{
    constexpr std::array names{"Front", "Right", "Top", "Back", "Left", "Bottom", "Isometric",
        "Arbitrary orientation", "Projection from view", "View by arrow", "Stepped section",
        "Aligned section", "Detail view", "Local section"};
    return QCoreApplication::translate("DrawingViews", names.at(static_cast<std::size_t>(kind)));
}

const View& baseModelView(const ModelViews& model, int index)
{
    const auto& views = model.geometry->views;
    return *std::array{&views.front, &views.right, &views.top}.at(index);
}

std::optional<LocatedView> locateModelView(const ModelViews& model, QUuid id)
{
    if (!model.geometry || id.isNull()) {
        return {};
    }
    for (int i = 0; i < 3; ++i) {
        const auto& base = baseModelView(model, i);
        if (base.id == id) {
            return LocatedView{&base, model.layout[i]};
        }
    }
    for (const auto& extra : model.additional) {
        if (extra.id == id && extra.geometry) {
            auto placement = extra.placement;
            if (extra.kind == DrawingViewKind::Detail) {
                placement.center -= QPointF(extra.focus.x(), -extra.focus.y()) * placement.scale;
            }
            return LocatedView{&extra.geometry->view, placement};
        }
    }
    return {};
}

void rebuildDrawingViews(ModelViews& model, const TopoDS_Shape& solid, const ModelViews* previous)
{
    if (model.additional.empty()) {
        return;
    }
    QHash<QUuid, ViewDirection> directions;
    for (int i = 0; i < 3; ++i) {
        directions.insert(baseModelView(model, i).id, standardDirection(i));
    }

    for (auto& spec : model.additional) {
        using enum DrawingViewKind;
        if (spec.id.isNull() || directions.contains(spec.id)) {
            throw std::invalid_argument("A drawing view has a duplicate identifier.");
        }
        const auto source = locateModelView(model, spec.source);
        if (derivedView(spec.kind) && (!source || !directions.contains(spec.source))) {
            throw std::invalid_argument("The source view for this projection is unavailable.");
        }
        ViewDirection direction;
        if (derivedView(spec.kind)) {
            direction = directions.value(spec.source);
        }
        auto computed = std::make_shared<CutProjection>();
        if (spec.kind == Detail) {
            computed->view = *source->view;
        } else if (spec.kind == Local) {
            *computed = projectLocalCut(solid, direction, spec.depth);
        } else if (pathView(spec.kind)) {
            if (spec.path.size() < 2) {
                throw std::invalid_argument("Enter the points defining the projection.");
            }
            const auto delta = spec.path[1] - spec.path[0];
            if (std::hypot(delta.x(), delta.y()) < 1.e-5) {
                throw std::invalid_argument("Cut path points must be distinct.");
            }
            if (spec.kind == SteppedSection || spec.kind == AlignedSection) {
                std::vector<Point> path;
                for (const auto p : spec.path) {
                    path.push_back({p.x(), p.y()});
                }
                *computed = projectCut(solid, direction, path, spec.kind == AlignedSection);
                direction = projectedDirection(direction, {delta.y(), -delta.x()});
            } else {
                const auto towardViewer = spec.kind == Arrow ? -delta : delta;
                direction = projectedDirection(direction, {towardViewer.x(), towardViewer.y()});
                computed->view = projectOriented(solid, direction);
            }
        } else {
            direction = spec.kind <= Bottom ? standardDirection(static_cast<int>(spec.kind))
                : spec.kind == Isometric ? angledDirection(45, 35.26438968275, 0)
                : angledDirection(spec.angles[0], spec.angles[1], spec.angles[2]);
            computed->view = projectOriented(solid, direction);
        }

        // Detail geometry is copied from the source so its edge identities follow it.
        if (spec.kind != Detail && previous) {
            const auto old = std::ranges::find(previous->additional, spec.id, &DrawingView::id);
            if (old != previous->additional.end() && old->geometry && old->kind == spec.kind
                && old->source == spec.source && old->angles == spec.angles && old->path == spec.path
                && old->depth == spec.depth) {
                retainProjectionReferences(computed->view, old->geometry->view);
            }
        }
        computed->view.id = spec.id;
        spec.geometry = std::move(computed);
        directions.insert(spec.id, direction);
    }
}

}  // namespace graph3d::drawing
