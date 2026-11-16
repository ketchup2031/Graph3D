#pragma once

#include "drawing/Draft.h"

#include <QPainterPath>

namespace graph3d::drawing {

inline constexpr double draftTolerance = 1.e-6;

struct DraftCurve {
    QPointF a;
    QPointF b;
    double radius = 0;
    double angle = 0;
    double sweep = 0;

    QPointF at(double t) const;
    std::optional<double> parameter(QPointF point) const;
    double nearest(QPointF point) const;
    double length() const;
};

struct DraftCrossing {
    double parameter;
    QPointF point;
};

DraftCurve draftArc(QPointF start, QPointF through, QPointF end);
std::vector<DraftCurve> draftCurves(const DraftItem& item);
QPainterPath draftCurvePath(const DraftItem& item);
std::vector<DraftCrossing> draftIntersections(const DraftCurve& first, const DraftCurve& second,
                                               bool extendFirst = false);
std::vector<DraftItem> trimDraft(const DraftItem& item, const Draft& draft, QPointF click);
std::optional<DraftItem> extendDraft(const DraftItem& item, const Draft& draft, QPointF click);
std::optional<DraftItem> offsetDraft(const DraftItem& item, double distance);
std::optional<DraftItem> hatchDraft(const Draft& draft, QPointF point);

}  // namespace graph3d::drawing
