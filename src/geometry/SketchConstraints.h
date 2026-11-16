#pragma once

#include "geometry/Sketch.h"

#include <span>
#include <string>

namespace graph3d::geometry {

struct SketchSolution {
    std::vector<SketchEntity> entities;
    std::vector<int> conflicts;
    std::vector<int> brokenProjections;
    std::string projectionError;
};

Point2 sketchVertex(const SketchEntity& entity, int part);
Line sketchSegment(const SketchEntity& entity, int part);
int sketchVertexCount(const SketchEntity& entity);
int sketchSegmentCount(const SketchEntity& entity);
void validateSketchConstraints(const Sketch& sketch);
SketchSolution solveSketch(const Sketch& sketch, std::span<const SketchProjection> projections = {});

}  // namespace graph3d::geometry
