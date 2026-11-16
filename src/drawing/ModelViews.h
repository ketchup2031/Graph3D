#pragma once

#include "drawing/Projection.h"

#include <QByteArray>
#include <QPointF>
#include <QSizeF>
#include <QString>

#include <array>
#include <memory>
#include <optional>

class QPainter;

namespace graph3d::drawing {

struct ViewPlacement {
    QPointF center;
    double scale;
    bool operator==(const ViewPlacement&) const = default;
};

// Placements are ordered front, right, top and measured in millimetres on paper.
using ViewLayout = std::array<ViewPlacement, 3>;

struct SectionSettings {
    geometry::Plane plane;
    double offset;
    bool operator==(const SectionSettings&) const = default;
};

struct ModelProjection {
    Projections views;
    std::optional<Section> section;
};

struct ModelViews {
    QString sourcePath;
    int bodyId = 0;
    QByteArray sourceDigest;
    ViewLayout layout{};
    bool dimensions = true;
    bool hiddenLines = true;
    std::shared_ptr<const ModelProjection> geometry;
};

ViewLayout defaultViewLayout(const Projections& views, QSizeF paper);
std::optional<SectionSettings> sectionSettings(const ModelViews& model);
void paintModelViews(QPainter& painter, const ModelViews& model);

}  // namespace graph3d::drawing
