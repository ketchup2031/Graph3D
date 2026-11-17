#pragma once

#include "drawing/Draft.h"

namespace graph3d::drawing {

std::optional<QPointF> modelReferencePoint(const ModelViews& model, const DraftReference& reference);
std::optional<double> modelReferenceScale(const ModelViews& model, const DraftReference& reference);

}  // namespace graph3d::drawing
