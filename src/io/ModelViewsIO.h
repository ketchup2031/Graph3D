#pragma once

#include "document/Document.h"
#include "drawing/ModelViews.h"

#include <QJsonObject>

namespace graph3d::io {

struct ModelSource {
    QString path;
    QByteArray digest;
    document::Document document;
};

ModelSource openModelSource(const QString& path);
QByteArray currentModelDigest(const QString& path);
bool canProjectBody(const document::Object& item);
drawing::ModelViews buildModelViews(const ModelSource& source, int bodyId,
                                    std::optional<drawing::SectionSettings> cut, QSizeF paper,
                                    const drawing::ModelViews* previous = nullptr);

drawing::ModelViews readModelViews(const QJsonObject& object, const QString& draftPath);
QJsonObject writeModelViews(const drawing::ModelViews& model, const QString& draftPath);

}  // namespace graph3d::io
