#pragma once

#include "drawing/Projection.h"

#include <QString>

class QPainter;
class QRectF;

namespace graph3d::drawing {

struct SheetOptions {
    QString title;
    QString designation;
    bool dimensions = true;
    bool hiddenLines = true;
};

inline constexpr double sheetWidth = 297;
inline constexpr double sheetHeight = 210;

void paintSheet(QPainter& painter, const QRectF& area, const Projections& views, const SheetOptions& options);
void savePdf(const QString& path, const Projections& views, const SheetOptions& options);

}  // namespace graph3d::drawing
