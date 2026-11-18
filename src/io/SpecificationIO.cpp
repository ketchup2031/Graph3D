#include "io/SpecificationIO.h"
#include "io/AssemblyDrawingIO.h"
#include "io/FileIO.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace graph3d::io {
namespace {
QString normalized(const QString& path)
{
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}
QString decodePath(const QDir& dir, const QString& path)
{
    return path.isEmpty() ? QString{} : QDir::cleanPath(dir.absoluteFilePath(path));
}
QString text(const QJsonValue& value)
{
    if (!value.isString()) { fileError(QStringLiteral("Invalid specification text field.")); }
    return value.toString();
}
int integer(const QJsonValue& value)
{
    const int result = value.toInt(-1);
    if (result < 0) { fileError(QStringLiteral("Invalid specification number.")); }
    return result;
}
}

drawing::SpecificationDocument openSpecification(const QString& path)
{
    const auto root = QJsonDocument::fromJson(readFile(path)).object();
    if (root.value("format") != QJsonValue("Graph3D Specification") || root.value("version").toInt() != 1
        || !root.value("rows").isArray() || !root.value("history").isObject()) {
        fileError(QStringLiteral("Unsupported Graph3D specification format."));
    }
    const auto base = QFileInfo(path).absoluteDir();
    drawing::SpecificationDocument spec;
    spec.title = text(root.value("title"));
    spec.designation = text(root.value("designation"));
    spec.sourcePath = decodePath(base, text(root.value("source")));
    spec.drawingPath = decodePath(base, text(root.value("drawing")));
    spec.assemblyId = integer(root.value("assembly"));
    spec.drawingSheet = integer(root.value("sheet"));
    spec.nextPosition = integer(root.value("next"));
    if (root.contains("variant")) { spec.variant = text(root.value("variant")); }
    const auto rows = root.value("rows").toArray();
    const auto history = root.value("history").toObject();
    if (rows.size() > 1000 || history.size() > 10000) { fileError(QStringLiteral("Specification exceeds the row limit.")); }
    for (const auto& row : rows) {
        const auto fields = row.toObject();
        spec.rows.push_back({text(fields.value("key")), text(fields.value("section")), text(fields.value("designation")),
            text(fields.value("name")), text(fields.value("material")), text(fields.value("note")),
            integer(fields.value("position")), integer(fields.value("quantity"))});
    }
    for (auto it = history.begin(); it != history.end(); ++it) { spec.history.emplace(it.key(), integer(it.value())); }
    drawing::validateSpecification(spec);
    return spec;
}

void saveSpecification(const QString& path, const drawing::SpecificationDocument& spec, bool recovery)
{
    drawing::validateSpecification(spec);
    const auto directory = QFileInfo(path).absoluteDir();
    const auto pathValue = [&](const QString& file) {
        return file.isEmpty() ? QString{} : recovery ? file : directory.relativeFilePath(file);
    };
    QJsonObject history;
    for (const auto& [key, number] : spec.history) { history.insert(key, number); }
    QJsonArray rows;
    for (const auto& row : spec.rows) {
        rows.append(QJsonObject{{"key", row.key}, {"section", row.section}, {"designation", row.designation},
            {"name", row.name}, {"material", row.material}, {"note", row.note}, {"position", row.position}, {"quantity", row.quantity}});
    }
    QJsonObject root{{"format", "Graph3D Specification"}, {"version", 1}, {"title", spec.title}, {"designation", spec.designation},
        {"source", pathValue(spec.sourcePath)}, {"assembly", spec.assemblyId}, {"drawing", pathValue(spec.drawingPath)},
        {"sheet", spec.drawingSheet}, {"next", spec.nextPosition}, {"rows", rows}, {"history", history}};
    if (spec.variant) { root.insert("variant", *spec.variant); }
    const auto data = QJsonDocument(root).toJson();
    if (data.size() > fileLimit) { fileError(QStringLiteral("Specification exceeds the file size limit.")); }
    writeFile(path, data);
}

std::shared_ptr<const drawing::AssemblyDrawing> readSpecificationAssembly(const drawing::SpecificationDocument& spec)
{
    if (spec.sourcePath.isEmpty()) { fileError(QStringLiteral("Link a source assembly first.")); }
    auto source = openModelSource(spec.sourcePath);
    if (spec.variant) { source.document.selectVariant(spec.variant->toStdString()); }
    const auto* body = source.document.find(spec.assemblyId);
    if (!body || !canProjectBody(*body)) { fileError(QStringLiteral("The linked assembly is unavailable.")); }
    const auto result = buildAssemblyDrawing(source, *body, nullptr);
    if (!result) { fileError(QStringLiteral("The selected model object is not an assembly.")); }
    return result;
}

bool specificationMatches(const drawing::SpecificationDocument& spec, const drawing::ModelViews& model)
{
    if (spec.sourcePath.isEmpty() || model.sourcePath.isEmpty() || !model.assembly
        || spec.assemblyId != model.bodyId || spec.variant != model.variant) { return false; }
#ifdef Q_OS_WIN
    return normalized(spec.sourcePath).compare(normalized(model.sourcePath), Qt::CaseInsensitive) == 0;
#else
    return normalized(spec.sourcePath) == normalized(model.sourcePath);
#endif
}

void attachSpecification(drawing::ModelViews& model, const QString& path, bool rebuild)
{
    const auto spec = openSpecification(path);
    if (!specificationMatches(spec, model)) {
        fileError(QStringLiteral("The specification and drawing must use the same assembly and variant."));
    }
    auto table = std::make_shared<drawing::AssemblyDrawing>(*model.assembly);
    if (rebuild) { drawing::synchronizeDrawingPositions(spec, *table); }
    model.assembly = std::move(table);
    model.specificationPath = normalized(path);
}

drawing::Draft* specificationSheet(drawing::Draft& draft, int sheet)
{
    if (sheet == 0) { return &draft; }
    return sheet > 0 && sheet <= static_cast<int>(draft.additionalSheets.size()) ? &draft.additionalSheets[sheet - 1] : nullptr;
}
}  // namespace graph3d::io
