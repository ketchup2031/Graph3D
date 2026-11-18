#pragma once

#include "document/Document.h"

#include <QString>

class QWidget;

namespace graph3d {

std::optional<document::Parameters> advancedParameters(QWidget* parent, const QString& title,
    const document::Document& document, const document::Parameters& initial, int editing);
std::optional<double> bodyPropertiesDialog(QWidget* parent, const document::Object& body);

}  // namespace graph3d
