#pragma once

#include <QSettings>

namespace graph3d {

QSettings userSettings();
bool darkThemeEnabled();
void applyTheme(bool dark);

}  // namespace graph3d
