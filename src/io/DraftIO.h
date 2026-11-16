#pragma once

#include "drawing/Draft.h"

namespace graph3d::io {

drawing::Draft openDraft(const QString& path);
void saveDraft(const QString& path, const drawing::Draft& draft);

}  // namespace graph3d::io
