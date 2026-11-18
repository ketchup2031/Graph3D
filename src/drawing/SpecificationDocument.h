#pragma once

#include "drawing/ModelViews.h"
#include <QString>
#include <QStringList>
#include <vector>

namespace graph3d::drawing {

struct SpecRow {
    QString key;   // Component family key, empty for a manual row.
    QString section, designation, name, material, note;
    int position = 0;
    int quantity = 1;
    bool operator==(const SpecRow&) const = default;
};
struct SpecificationDocument {
    QString title, designation;
    QString sourcePath, drawingPath;
    int assemblyId = 0, drawingSheet = 0; // Sheet is zero-based.
    std::optional<QString> variant;
    std::vector<SpecRow> rows;
    std::map<QString, int> history;
    int nextPosition = 1;
    bool operator==(const SpecificationDocument&) const = default;
};

void validateSpecification(const SpecificationDocument& spec);
void mergeSpecification(SpecificationDocument& spec, const AssemblyDrawing& model);
QStringList auditSpecification(const SpecificationDocument& spec, const AssemblyDrawing* model, const ModelViews* drawing);
void synchronizeDrawingPositions(const SpecificationDocument& spec, AssemblyDrawing& drawing);
void exportSpecificationPdf(const QString& path, const SpecificationDocument& spec);
QByteArray specificationCsv(const SpecificationDocument& spec);

}  // namespace graph3d::drawing
