#pragma once

#include "drawing/Draft.h"
#include <QMap>

namespace graph3d::io {

struct DxfDrawing {
    drawing::Draft drawing;
    int skipped = 0;
    bool beyondSheet = false;
    QMap<QString,int> losses;
};

DxfDrawing importDxf(const QString& path);
int exportDxf(const QString& path, const drawing::Draft& sheet);
QMap<QString,int> exportDxfReport(const QString& path, const drawing::Draft& sheet, bool allSheets = false);
DxfDrawing importDwg(const QString& path);
QMap<QString,int> exportDwg(const QString& path, const drawing::Draft& sheet);

}  // namespace graph3d::io
