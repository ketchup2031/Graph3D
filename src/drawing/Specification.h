#pragma once

#include "drawing/ModelViews.h"

#include <QStringList>

namespace graph3d::drawing {

using SpecificationCells = std::array<QString, 6>;

struct SpecificationBand {
    std::array<QStringList, 6> cells;
    int lines;
    bool section = false;
};
using SpecificationPage = std::vector<SpecificationBand>;

SpecificationCells specificationHeaders();
SpecificationCells specificationCells(const SpecificationRow& row);
std::vector<SpecificationPage> paginateSpecification(const AssemblyDrawing& assembly);
void paintSpecificationPage(QPainter& painter, const AssemblyDrawing& assembly,
                            const SpecificationPage& page, int sheet, int index, int count);
QPointF positionOrigin(const ModelViews& model, const SpecificationRow& row, int instance, int view);
void paintPositionCallouts(QPainter& painter, const ModelViews& model);

}  // namespace graph3d::drawing
