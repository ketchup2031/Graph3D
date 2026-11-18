#pragma once

#include "drawing/Draft.h"
#include "drawing/SpecificationDocument.h"
#include "io/ModelViewsIO.h"

namespace graph3d::io {
drawing::SpecificationDocument openSpecification(const QString& path);
void saveSpecification(const QString& path, const drawing::SpecificationDocument& spec, bool recovery = false);
std::shared_ptr<const drawing::AssemblyDrawing> readSpecificationAssembly(const drawing::SpecificationDocument& spec);
bool specificationMatches(const drawing::SpecificationDocument& spec, const drawing::ModelViews& model);
void attachSpecification(drawing::ModelViews& model, const QString& path, bool rebuild = true);
drawing::Draft* specificationSheet(drawing::Draft& draft, int sheet);
}  // namespace graph3d::io
