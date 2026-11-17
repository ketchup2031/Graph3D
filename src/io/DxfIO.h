#pragma once

#include "drawing/Draft.h"

namespace graph3d::io {

struct DxfDrawing {
    drawing::Draft drawing;
    int skipped = 0;
    bool beyondSheet = false;
};

DxfDrawing importDxf(const QString& path);
int exportDxf(const QString& path, const drawing::Draft& sheet);

}  // namespace graph3d::io
