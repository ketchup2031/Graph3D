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

constexpr std::array kinds{"line", "rectangle", "circle", "text", "horizontal", "vertical"};
constexpr std::array strokes{"solid", "hidden", "center"};
constexpr std::array anchors{"start", "end", "corner-ab", "corner-ba", "center", "right", "left", "top", "bottom"};
constexpr int maxItems = 20000;

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
    return drawing::DraftReference{readId(anchor.value("item")),
                                    readName<drawing::DraftAnchor>(anchor.value("point"), anchors)};
}

QJsonObject writeAnchor(const drawing::DraftReference& anchor)
{
    return {{"item", anchor.item.toString(QUuid::WithoutBraces)},
            {"point", anchors.at(static_cast<std::size_t>(anchor.anchor))}};
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
        if ((item.anchorA || item.anchorB) && !drawing::isLinearDimension(item.kind)) {
            fileError(QStringLiteral("Only linear dimensions can have attached points."));
        }
        if ((item.anchorA && !geometry.point(*item.anchorA)) ||
            (item.anchorB && !geometry.point(*item.anchorB))) {
            fileError(QStringLiteral("Invalid dimension attachment."));
        }
    }
}

}  // namespace

drawing::Draft openDraft(const QString& path)
{
    const auto root = QJsonDocument::fromJson(readFile(path)).object();
    const auto version = root.value("version").toDouble();
    if (root.value("format") != QJsonValue("Graph3D Drawing") || (version != 1 && version != 2 && version != 3)
        || root.value("unit") != QJsonValue("mm")) {
        fileError(QStringLiteral("This is not a supported Graph3D drawing."));
    }
    const auto paper = root.value("paper").toString();
    const auto items = root.value("items").toArray();
    if ((paper != "A4" && paper != "A3") || !root.value("items").isArray() || items.size() > maxItems) {
        fileError(QStringLiteral("Invalid drawing sheet or item list."));
    }
    drawing::Draft draft{readText(root.value("title")), readText(root.value("number")),
                         readText(root.value("author")), paper == "A3", {}};
    draft.items.reserve(items.size());
    for (const auto value : items) {
        const auto entry = value.toObject();
        drawing::DraftItem item{readName<drawing::DraftKind>(entry.value("type"), kinds),
            readPoint(entry.value("a")), readPoint(entry.value("b")), readPoint(entry.value("label")),
            readText(entry.value("text")), readName<drawing::DraftStroke>(entry.value("stroke"), strokes)};
        if (!drawing::validItem(item)) {
            fileError(QStringLiteral("Invalid drawing geometry."));
        }
        item.id = version >= 2 ? readId(entry.value("id")) : QUuid::createUuid();
        if (version >= 2) {
            item.anchorA = readAnchor(entry.value("anchorA"));
            item.anchorB = readAnchor(entry.value("anchorB"));
        }
        draft.items.push_back(std::move(item));
    }
    if (version >= 2) {
        validateLinks(draft);
    }
    if (version == 3) {
        draft.modelViews = readModelViews(root.value("modelViews").toObject(), path);
    }
    return draft;
}

void saveDraft(const QString& path, const drawing::Draft& draft)
{
    if (draft.items.size() > maxItems || draft.title.size() > 200 || draft.number.size() > 200
        || draft.author.size() > 200) {
        fileError(QStringLiteral("This drawing exceeds the file limits."));
    }
    const bool linked = std::ranges::any_of(draft.items, [](const auto& item) { return item.anchorA || item.anchorB; });
    if (linked) {
        validateLinks(draft);
    }
    const int version = draft.modelViews ? 3 : linked ? 2 : 1;
    QJsonArray items;
    for (const auto& item : draft.items) {
        if (!drawing::validItem(item) || item.text.size() > 200) {
            fileError(QStringLiteral("Invalid drawing item."));
        }
        QJsonObject entry{{"type", kinds.at(static_cast<std::size_t>(item.kind))},
            {"stroke", strokes.at(static_cast<std::size_t>(item.stroke))},
            {"a", writePoint(item.a)}, {"b", writePoint(item.b)}, {"label", writePoint(item.label)},
            {"text", item.text}};
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
        items.append(entry);
    }
    QJsonObject root{{"format", "Graph3D Drawing"}, {"version", version}, {"unit", "mm"},
        {"paper", draft.a3 ? "A3" : "A4"}, {"title", draft.title}, {"number", draft.number},
        {"author", draft.author}, {"items", items}};
    if (draft.modelViews) {
        root.insert("modelViews", writeModelViews(*draft.modelViews, path));
    }
    const auto data = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (data.size() > fileLimit) {
        fileError(QStringLiteral("This drawing exceeds the file limits."));
    }
    writeFile(path, data);
}

}  // namespace graph3d::io
