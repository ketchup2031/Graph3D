#include "io/FileIO.h"

#include <QFile>
#include <QSaveFile>

#include <stdexcept>

namespace graph3d::io {

void fileError(const QString& message)
{
    throw std::runtime_error(message.toUtf8().toStdString());
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fileError(file.errorString());
    }
    const auto data = file.read(fileLimit + 1);
    if (file.error() != QFileDevice::NoError) {
        fileError(file.errorString());
    }
    if (data.size() > fileLimit) {
        fileError(QStringLiteral("File exceeds the 64 MiB read limit."));
    }
    return data;
}

void writeFile(const QString& path, const QByteArray& data)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        fileError(file.errorString());
    }
}

}  // namespace graph3d::io
