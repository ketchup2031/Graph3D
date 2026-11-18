#pragma once

#include "document/Document.h"

#include <QString>

namespace graph3d::io {

enum class ExchangeFormat { Step, Stl, Iges };

void exportVisible(const QString& path, const document::Document& document, ExchangeFormat format);
TopoDS_Shape readStep(const QString& path);
std::vector<document::ExchangeNode> readStepTree(const QString& path);
TopoDS_Shape readIges(const QString& path);
document::ImportedMesh readStl(const QString& path, double millimetresPerUnit = 1);

}  // namespace graph3d::io
