#pragma once

#include "document/Document.h"

#include <QByteArray>
#include <QString>

namespace graph3d::io {

void saveProject(const QString& path, const document::Document& document);
document::Document decodeProject(const QByteArray& data, const QString& path = {});
document::Document openProject(const QString& path);

}  // namespace graph3d::io
