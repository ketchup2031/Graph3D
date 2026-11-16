#pragma once

#include "document/Document.h"

#include <QString>

namespace graph3d::io {

enum class ExchangeFormat { Step, Stl };

void exportVisible(const QString& path, const document::Document& document, ExchangeFormat format);
TopoDS_Shape readStep(const QString& path);

}  // namespace graph3d::io
