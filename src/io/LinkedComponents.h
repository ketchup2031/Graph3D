#pragma once

#include "document/Document.h"

#include <QString>

namespace graph3d::io {

document::LinkedComponent readLinkedComponent(const QString& source, int object, const QString& owner = {});
QString portableComponentIdentity(const document::Object& object, const QString& owner);
bool updateLinkedComponents(document::Document& document, const QString& owner, int selected = 0);

}  // namespace graph3d::io
