#pragma once
#include "drawing/Draft.h"
#include "geometry/SheetMetal.h"
namespace graph3d::io {
drawing::Draft flatDrawing(const geometry::SheetMetal& sheet);
drawing::Draft refreshFlatDrawing(drawing::Draft draft);
}
