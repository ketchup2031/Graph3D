#pragma once

#include "io/ModelViewsIO.h"

namespace graph3d::io {

std::shared_ptr<const drawing::AssemblyDrawing> buildAssemblyDrawing(
    const ModelSource& source, const document::Object& object, const drawing::AssemblyDrawing* previous);
drawing::AssemblyDrawing readAssemblyDrawing(const QJsonValue& value);
QJsonObject writeAssemblyDrawing(const drawing::AssemblyDrawing& assembly);

}  // namespace graph3d::io
