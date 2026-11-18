#include "io/DraftIO.h"

#include "io/FileIO.h"
#include "io/ModelViewsIO.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <array>

namespace graph3d::io {
namespace {

constexpr std::array kinds{"line", "rectangle", "circle", "text", "horizontal", "vertical", "arc", "polyline",
                           "aligned", "angular", "radius", "diameter", "hatch", "leader", "datum", "tolerance", "roughness", "ellipse", "spline",
                           "construction-line", "fragment", "table", "thread", "weld"};
constexpr std::array strokes{"solid", "hidden", "center"};
constexpr std::array anchors{"start", "end", "corner-ab", "corner-ba", "center", "right", "left", "top", "bottom", "curve", "intersection", "model-curve", "model-center"};
constexpr int maxItems = 20000;
constexpr int maxDraftPoints = 750000;

template<class Enum, std::size_t Size>
Enum readName(const QJsonValue& value, const std::array<const char*, Size>& names)
{
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (value.toString() == QLatin1String(names[i])) {
            return static_cast<Enum>(i);
        }
    }
    fileError(QStringLiteral("Unknown drawing item or line style."));
}

QString readText(const QJsonValue& value)
{
    if (!value.isString() || value.toString().size() > 200) {
        fileError(QStringLiteral("Invalid drawing text."));
    }
    return value.toString();
}

QPointF readPoint(const QJsonValue& value)
{
    const auto coordinates = value.toArray();
    if (coordinates.size() != 2 || !coordinates[0].isDouble() || !coordinates[1].isDouble()) {
        fileError(QStringLiteral("Invalid drawing coordinates."));
    }
    return {coordinates[0].toDouble(), coordinates[1].toDouble()};
}

QJsonArray writePoint(QPointF point)
{
    return {point.x(), point.y()};
}

std::vector<QPointF> readPoints(const QJsonValue& value, int& remaining)
{
    const auto array = value.toArray();
    if (!value.isArray() || array.size() > remaining) {
        fileError(QStringLiteral("Invalid drawing coordinates."));
    }
    remaining -= static_cast<int>(array.size());
    std::vector<QPointF> points;
    points.reserve(array.size());
    for (const auto point : array) {
        points.push_back(readPoint(point));
    }
    return points;
}

QJsonArray writePoints(const std::vector<QPointF>& points, int& remaining)
{
    if (points.size() > static_cast<std::size_t>(remaining)) {
        fileError(QStringLiteral("This drawing exceeds the file limits."));
    }
    remaining -= static_cast<int>(points.size());
    QJsonArray out;
    for (const auto point : points) {
        out.append(writePoint(point));
    }
    return out;
}

QUuid readId(const QJsonValue& value)
{
    const QUuid id(value.toString());
    if (id.isNull()) {
        fileError(QStringLiteral("Invalid drawing item ID."));
    }
    return id;
}

std::optional<drawing::DraftReference> readAnchor(const QJsonValue& value)
{
    if (value.isUndefined()) {
        return {};
    }
    const auto anchor = value.toObject();
    drawing::DraftReference reference{readId(anchor.value("item")),
        readName<drawing::DraftAnchor>(anchor.value("point"), anchors)};
    if (reference.anchor == drawing::DraftAnchor::Curve || reference.anchor == drawing::DraftAnchor::Intersection) {
        reference.parameter = anchor.value("parameter").toDouble(-1);
    }
    if (reference.anchor == drawing::DraftAnchor::Intersection) {
        reference.crossing = readId(anchor.value("crossing"));
    }
    if (drawing::isModelReference(reference)) {
        reference.view = readId(anchor.value("view"));
        reference.parameter = anchor.value("parameter").toDouble(-1);
    }
    return reference;
}

QJsonObject writeAnchor(const drawing::DraftReference& anchor)
{
    QJsonObject out{{"item", anchor.item.toString(QUuid::WithoutBraces)},
                    {"point", anchors.at(static_cast<std::size_t>(anchor.anchor))}};
    if (anchor.anchor == drawing::DraftAnchor::Curve || anchor.anchor == drawing::DraftAnchor::Intersection) {
        out.insert("parameter", anchor.parameter);
    }
    if (anchor.anchor == drawing::DraftAnchor::Intersection) {
        out.insert("crossing", anchor.crossing.toString(QUuid::WithoutBraces));
    }
    if (drawing::isModelReference(anchor)) {
        out.insert("view", anchor.view.toString(QUuid::WithoutBraces));
        out.insert("parameter", anchor.parameter);
    }
    return out;
}

void validateLinks(const drawing::Draft& draft)
{
    QSet<QUuid> ids;
    const drawing::DraftGeometry geometry(draft);
    for (const auto& item : draft.items) {
        if (item.id.isNull() || ids.contains(item.id)) {
            fileError(QStringLiteral("Invalid or duplicate drawing item ID."));
        }
        ids.insert(item.id);
        if (((item.anchorA || item.anchorB) && !drawing::isDimension(item.kind))
            || (item.anchorC && item.kind != drawing::DraftKind::Angular)) {
            fileError(QStringLiteral("Invalid dimension attachment."));
        }
        for (const auto& anchor : {item.anchorA, item.anchorB, item.anchorC}) {
            if (anchor && !geometry.validReference(*anchor)) {
                fileError(QStringLiteral("Invalid dimension attachment."));
            }
        }
    }
}

drawing::DraftItem decodeItem(const QJsonObject& entry, int version, int& remaining, bool child = false)
{
    using namespace drawing;
    DraftItem item{readName<DraftKind>(entry.value("type"), kinds), readPoint(entry.value("a")),
        readPoint(entry.value("b")), readPoint(entry.value("label")), readText(entry.value("text")),
        readName<DraftStroke>(entry.value("stroke"), strokes)};
    if (version >= 4) {
        if (entry.contains("points")) {
            item.points = readPoints(entry.value("points"), remaining);
        }
        if (entry.contains("loops")) {
            if (!entry.value("loops").isArray()) {
                fileError(QStringLiteral("Invalid drawing coordinates."));
            }
            for (const auto loop : entry.value("loops").toArray()) {
                item.loops.push_back(readPoints(loop, remaining));
            }
        }
        item.anchorC = readAnchor(entry.value("anchorC"));
    } else if (item.kind > DraftKind::Vertical) {
        fileError(QStringLiteral("Invalid drawing geometry."));
    }
    item.id = version >= 2 ? readId(entry.value("id")) : QUuid::createUuid();
    if (version >= 2) {
        item.anchorA = readAnchor(entry.value("anchorA"));
        item.anchorB = readAnchor(entry.value("anchorB"));
    }
    if (version >= 5) {
        item.layer = entry.value("layer").toInt(-1);
    }
    if ((version < 5 && isAnnotation(item.kind)) || (version < 7 && item.kind > DraftKind::Roughness)) {
        fileError(QStringLiteral("Invalid drawing geometry."));
    }
    if (version >= 7) {
        if (entry.contains("weldReverse") && !entry.value("weldReverse").isBool()) {
            fileError(QStringLiteral("Invalid drawing item."));
        }
        item.weldReverse = entry.value("weldReverse").toBool();
        if (entry.contains("hatchBoundaries")) {
            const auto loops = entry.value("hatchBoundaries").toArray();
            if (loops.size() != static_cast<qsizetype>(item.loops.size())) {
                fileError(QStringLiteral("Invalid hatch boundary attachment."));
            }
            for (const auto loop : loops) {
                auto& ids = item.hatchBoundaries.emplace_back();
                for (const auto id : loop.toArray()) {
                    ids.push_back(readId(id));
                }
            }
        }
        if (entry.contains("fragment")) {
            const auto fragment = entry.value("fragment").toArray();
            if (child || fragment.isEmpty() || fragment.size() > 1024) {
                fileError(QStringLiteral("Invalid drawing fragment."));
            }
            for (const auto piece : fragment) {
                item.fragment.push_back(decodeItem(piece.toObject(), version, remaining, true));
            }
        }
        if (entry.contains("table")) {
            const auto data = entry.value("table").toObject();
            const auto cells = data.value("cells").toArray();
            if (cells.size() > 300) {
                fileError(QStringLiteral("Invalid drawing table."));
            }
            DraftTable table{data.value("columns").toInt(), data.value("width").toDouble(),
                             data.value("rowHeight").toDouble(), {}};
            for (const auto cell : cells) {
                table.cells << readText(cell);
            }
            item.table = std::move(table);
        }
        if (entry.contains("dimensionNote")) {
            const auto note = entry.value("dimensionNote").toObject();
            item.dimensionNote = {readText(note.value("fit")), readText(note.value("upper")), readText(note.value("lower"))};
        }
    }
    if (!validItem(item) || (child && item.layer != 0)) {
        fileError(QStringLiteral("Invalid drawing geometry."));
    }
    return item;
}

drawing::TitleBlockLayout readTitleBlock(const QJsonObject& object)
{
    drawing::TitleBlockLayout layout;
    layout.width = object.value("width").toDouble();
    layout.rowHeight = object.value("rowHeight").toDouble();
    for (const auto value : object.value("rows").toArray()) {
        const auto row = value.toObject();
        const int field = row.value("field").toInt(-1);
        if (field < 0 || field > static_cast<int>(drawing::TitleField::Pages)) {
            fileError(QStringLiteral("Invalid drawing title block."));
        }
        layout.rows.push_back({readText(row.value("label")), static_cast<drawing::TitleField>(field),
                               readText(row.value("text"))});
    }
    if (!drawing::validTitleBlock(layout)) {
        fileError(QStringLiteral("Invalid drawing title block."));
    }
    return layout;
}

drawing::Draft decodeSheet(const QJsonObject& root, int version, const QString& path, int& remaining)
{
    const auto paper = root.value("paper").toString();
    const auto items = root.value("items").toArray();
    if ((paper != "A4" && paper != "A3") || !root.value("items").isArray() || items.size() > maxItems) {
        fileError(QStringLiteral("Invalid drawing sheet or item list."));
    }
    drawing::Draft draft{readText(root.value("title")), readText(root.value("number")),
                         readText(root.value("author")), paper == "A3", {}};
    draft.items.reserve(items.size());
    if (version >= 5) {
        if (!root.value("portrait").isBool() || !root.value("eskd").isBool()
            || !root.value("requirements").isString() || root.value("requirements").toString().size() > 2400) {
            fileError(QStringLiteral("Invalid drawing sheet settings."));
        }
        draft.paper = {root.value("portrait").toBool(), root.value("eskd").toBool(),
            readText(root.value("material")), readText(root.value("organization")), root.value("requirements").toString()};
        const auto layers = root.value("layers").toArray();
        if (layers.isEmpty() || layers.size() > drawing::maxLayers) {
            fileError(QStringLiteral("Invalid drawing layers."));
        }
        if (draft.paper.eskd && !draft.a3 && !draft.paper.portrait) {
            fileError(QStringLiteral("ESKD drawings on A4 require portrait orientation."));
        }
        draft.layers.clear();
        for (const auto value : layers) {
            const auto layer = value.toObject();
            if (!layer.value("visible").isBool() || !layer.value("locked").isBool()) {
                fileError(QStringLiteral("Invalid drawing layers."));
            }
            draft.layers.push_back({readText(layer.value("name")), layer.value("visible").toBool(),
                                    layer.value("locked").toBool()});
        }
    }
    if (version >= 7) {
        if (root.contains("modelTitle") && !root.value("modelTitle").isBool()) {
            fileError(QStringLiteral("Invalid drawing title block."));
        }
        draft.paper.modelTitle = root.value("modelTitle").toBool();
        if (root.contains("titleBlock")) {
            draft.paper.titleBlock = readTitleBlock(root.value("titleBlock").toObject());
        }
    }
    for (const auto value : items) {
        auto item = decodeItem(value.toObject(), version, remaining);
        if (item.layer < 0 || item.layer >= static_cast<int>(draft.layers.size())) {
            fileError(QStringLiteral("Invalid drawing geometry."));
        }
        draft.items.push_back(std::move(item));
    }
    if (version >= 2) {
        validateLinks(draft);
    }
    if (version == 3 || (version >= 4 && root.contains("modelViews"))) {
        const auto model = root.value("modelViews").toObject();
        if (version < 6 && model.contains("assembly")) {
            fileError(QStringLiteral("This assembly drawing requires drawing format version 6."));
        }
        if (version < 8 && (model.contains("variant") || model.contains("threads"))) {
            fileError(QStringLiteral("Model variants and thread callouts require drawing format version 8."));
        }
        draft.modelViews = readModelViews(model, path);
    }
    return draft;
}

QJsonObject encodeItem(const drawing::DraftItem& item, int version, int& remaining)
{
    if (!drawing::validItem(item) || item.text.size() > 200) {
        fileError(QStringLiteral("Invalid drawing item."));
    }
    QJsonObject entry{{"type", kinds.at(static_cast<std::size_t>(item.kind))},
        {"stroke", strokes.at(static_cast<std::size_t>(item.stroke))},
        {"a", writePoint(item.a)}, {"b", writePoint(item.b)}, {"label", writePoint(item.label)}, {"text", item.text}};
    if (version >= 5) {
        entry.insert("layer", item.layer);
    }
    if (version >= 2) {
        const auto id = item.id.isNull() ? QUuid::createUuid() : item.id;
        entry.insert("id", id.toString(QUuid::WithoutBraces));
        if (item.anchorA) {
            entry.insert("anchorA", writeAnchor(*item.anchorA));
        }
        if (item.anchorB) {
            entry.insert("anchorB", writeAnchor(*item.anchorB));
        }
    }
    if (version >= 4) {
        if (item.anchorC) {
            entry.insert("anchorC", writeAnchor(*item.anchorC));
        }
        if (!item.points.empty()) {
            entry.insert("points", writePoints(item.points, remaining));
        }
        if (!item.loops.empty()) {
            QJsonArray loops;
            for (const auto& loop : item.loops) {
                loops.append(writePoints(loop, remaining));
            }
            entry.insert("loops", loops);
        }
    }
    if (!item.hatchBoundaries.empty()) {
        QJsonArray loops;
        for (const auto& loop : item.hatchBoundaries) {
            QJsonArray ids;
            for (const auto& id : loop) {
                ids.append(id.toString(QUuid::WithoutBraces));
            }
            loops.append(ids);
        }
        entry.insert("hatchBoundaries", loops);
    }
    if (!item.fragment.empty()) {
        QJsonArray pieces;
        for (const auto& child : item.fragment) {
            if (child.layer != 0) {
                fileError(QStringLiteral("Invalid drawing fragment."));
            }
            pieces.append(encodeItem(child, version, remaining));
        }
        entry.insert("fragment", pieces);
    }
    if (item.table) {
        QJsonArray cells;
        for (const auto& cell : item.table->cells) {
            cells.append(cell);
        }
        entry.insert("table", QJsonObject{{"columns", item.table->columns}, {"width", item.table->width},
                                         {"rowHeight", item.table->rowHeight}, {"cells", cells}});
    }
    if (item.weldReverse) {
        entry.insert("weldReverse", true);
    }
    if (item.dimensionNote != drawing::DimensionNote{}) {
        const auto& note = item.dimensionNote;
        entry.insert("dimensionNote", QJsonObject{{"fit", note.fit}, {"upper", note.upper}, {"lower", note.lower}});
    }
    return entry;
}

QJsonObject writeTitleBlock(const drawing::TitleBlockLayout& layout)
{
    if (!drawing::validTitleBlock(layout)) {
        fileError(QStringLiteral("Invalid drawing title block."));
    }
    QJsonArray rows;
    for (const auto& row : layout.rows) {
        rows.append(QJsonObject{{"label", row.label}, {"field", static_cast<int>(row.field)}, {"text", row.text}});
    }
    return {{"width", layout.width}, {"rowHeight", layout.rowHeight}, {"rows", rows}};
}

QJsonObject encodeSheet(const QString& path, const drawing::Draft& draft, int version, int& remaining)
{
    if (draft.items.size() > maxItems || draft.title.size() > 200 || draft.number.size() > 200
        || draft.author.size() > 200) {
        fileError(QStringLiteral("This drawing exceeds the file limits."));
    }
    const bool linked = std::ranges::any_of(draft.items, [](const auto& item) { return item.anchorA || item.anchorB || item.anchorC; });
    if (linked) {
        validateLinks(draft);
    }
    QJsonArray items;
    for (const auto& item : draft.items) {
        if (item.layer < 0 || item.layer >= static_cast<int>(draft.layers.size())) {
            fileError(QStringLiteral("Invalid drawing item."));
        }
        items.append(encodeItem(item, version, remaining));
    }
    QJsonObject root{{"paper", draft.a3 ? "A3" : "A4"}, {"title", draft.title}, {"number", draft.number},
        {"author", draft.author}, {"items", items}};
    if (draft.modelViews) {
        root.insert("modelViews", writeModelViews(*draft.modelViews, path, version >= 7));
    }
    if (version >= 5) {
        if (draft.layers.empty() || draft.layers.size() > drawing::maxLayers || draft.paper.material.size() > 200
            || draft.paper.organization.size() > 200 || draft.paper.requirements.size() > 2400) {
            fileError(QStringLiteral("Invalid drawing sheet settings."));
        }
        if (draft.paper.eskd && !draft.a3 && !draft.paper.portrait) {
            fileError(QStringLiteral("ESKD drawings on A4 require portrait orientation."));
        }
        QJsonArray layers;
        for (const auto& layer : draft.layers) {
            if (layer.name.size() > 200) {
                fileError(QStringLiteral("Invalid drawing layers."));
            }
            layers.append(QJsonObject{{"name", layer.name}, {"visible", layer.visible}, {"locked", layer.locked}});
        }
        root.insert("portrait", draft.paper.portrait);
        root.insert("eskd", draft.paper.eskd);
        root.insert("material", draft.paper.material);
        root.insert("organization", draft.paper.organization);
        root.insert("requirements", draft.paper.requirements);
        root.insert("layers", layers);
    }
    if (draft.paper.modelTitle) {
        root.insert("modelTitle", true);
    }
    if (draft.paper.titleBlock) {
        root.insert("titleBlock", writeTitleBlock(*draft.paper.titleBlock));
    }
    return root;
}

int draftVersion(const drawing::Draft& draft)
{
    const auto variant = [](const auto& sheet) {
        return sheet.modelViews && (sheet.modelViews->variant.has_value() || !sheet.modelViews->threads.empty());
    };
    if (variant(draft) || std::ranges::any_of(draft.additionalSheets, variant)) { return 8; }
    const auto needsCurrentVersion = [](const auto& sheet) {
        const auto model = [](const auto& anchor) { return anchor && drawing::isModelReference(*anchor); };
        if (sheet.paper.modelTitle || sheet.paper.titleBlock
            || (sheet.modelViews && (!sheet.modelViews->additional.empty()
                || std::ranges::find(sheet.modelViews->showBase, false) != sheet.modelViews->showBase.end()))) {
            return true;
        }
        return std::ranges::any_of(sheet.items, [&](const auto& item) {
            return model(item.anchorA) || model(item.anchorB) || model(item.anchorC)
                || item.kind > drawing::DraftKind::Roughness || !item.hatchBoundaries.empty()
                || item.dimensionNote != drawing::DimensionNote{};
        });
    };
    if (needsCurrentVersion(draft) || std::ranges::any_of(draft.additionalSheets, needsCurrentVersion)) {
        return 7;
    }
    const auto isAssembly = [](const auto& sheet) { return sheet.modelViews && sheet.modelViews->assembly; };
    if (isAssembly(draft) || std::ranges::any_of(draft.additionalSheets, isAssembly)) {
        return 6;
    }
    if (!draft.additionalSheets.empty() || draft.paper != drawing::DraftPaper{}
        || draft.layers != std::vector<drawing::DraftLayer>{{}}
        || (draft.modelViews && (draft.modelViews->detail || draft.modelViews->local))
        || std::ranges::any_of(draft.items, [](const auto& item) {
            return item.layer != 0 || drawing::isAnnotation(item.kind);
        })) {
        return 5;
    }
    const auto curveAnchor = [](const auto& reference) {
        return reference && (reference->anchor == drawing::DraftAnchor::Curve
                          || reference->anchor == drawing::DraftAnchor::Intersection);
    };
    const bool modern = std::ranges::any_of(draft.items, [&](const auto& item) {
        return item.kind > drawing::DraftKind::Vertical || curveAnchor(item.anchorA) || curveAnchor(item.anchorB);
    });
    const bool linked = std::ranges::any_of(draft.items, [](const auto& item) { return item.anchorA || item.anchorB || item.anchorC; });
    return modern ? 4 : draft.modelViews ? 3 : linked ? 2 : 1;
}

}  // namespace

drawing::Draft openDraft(const QString& path)
{
    const auto root = QJsonDocument::fromJson(readFile(path)).object();
    const int version = root.value("version").toInt();
    if (root.value("format") != QJsonValue("Graph3D Drawing") || version < 1 || version > 8
        || root.value("unit") != QJsonValue("mm")) {
        fileError(QStringLiteral("This is not a supported Graph3D drawing."));
    }
    int remaining = maxDraftPoints;
    if (version < 5) {
        return decodeSheet(root, version, path, remaining);
    }
    const auto sheets = root.value("sheets").toArray();
    if (sheets.isEmpty() || sheets.size() > drawing::maxSheets) {
        fileError(QStringLiteral("Invalid drawing sheet or item list."));
    }
    auto draft = decodeSheet(sheets[0].toObject(), version, path, remaining);
    for (qsizetype i = 1; i < sheets.size(); ++i) {
        draft.additionalSheets.push_back(decodeSheet(sheets[i].toObject(), version, path, remaining));
    }
    return draft;
}

void saveDraft(const QString& path, const drawing::Draft& draft)
{
    const int version = draftVersion(draft);
    int remaining = maxDraftPoints;
    QJsonObject root;
    if (version < 5) {
        root = encodeSheet(path, draft, version, remaining);
    } else {
        if (draft.additionalSheets.size() >= drawing::maxSheets) {
            fileError(QStringLiteral("Invalid drawing sheet or item list."));
        }
        QJsonArray sheets{encodeSheet(path, draft, version, remaining)};
        for (const auto& sheet : draft.additionalSheets) {
            if (!sheet.additionalSheets.empty()) {
                fileError(QStringLiteral("Invalid drawing sheet or item list."));
            }
            sheets.append(encodeSheet(path, sheet, version, remaining));
        }
        root.insert("sheets", sheets);
    }
    root.insert("format", "Graph3D Drawing");
    root.insert("version", version);
    root.insert("unit", "mm");
    const auto data = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (data.size() > fileLimit) {
        fileError(QStringLiteral("This drawing exceeds the file limits."));
    }
    writeFile(path, data);
}

}  // namespace graph3d::io
