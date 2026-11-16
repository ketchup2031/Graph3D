#pragma once

#include <QByteArray>
#include <QString>

namespace graph3d::io {

inline constexpr qsizetype fileLimit = 64 * 1024 * 1024;

[[noreturn]] void fileError(const QString& message);
QByteArray readFile(const QString& path);
void writeFile(const QString& path, const QByteArray& data);

}  // namespace graph3d::io
