#include "io/DraftIO.h"

#include "io/FileIO.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>

namespace graph3d::io {
namespace {

constexpr std::array kinds{"line", "rectangle", "circle", "text", "horizontal", "vertical"};
constexpr std::array strokes{"solid", "hidden", "center"};
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

}  // namespace

drawing::Draft openDraft(const QString& path)
{
    const auto root = QJsonDocument::fromJson(readFile(path)).object();
    if (root.value("format") != QJsonValue("Graph3D Drawing") || root.value("version") != QJsonValue(1)
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
        draft.items.push_back(std::move(item));
    }
    return draft;
}

void saveDraft(const QString& path, const drawing::Draft& draft)
{
    if (draft.items.size() > maxItems || draft.title.size() > 200 || draft.number.size() > 200
        || draft.author.size() > 200) {
        fileError(QStringLiteral("This drawing exceeds the file limits."));
    }
    QJsonArray items;
    for (const auto& item : draft.items) {
        if (!drawing::validItem(item) || item.text.size() > 200) {
            fileError(QStringLiteral("Invalid drawing item."));
        }
        items.append(QJsonObject{{"type", kinds.at(static_cast<std::size_t>(item.kind))},
            {"stroke", strokes.at(static_cast<std::size_t>(item.stroke))},
            {"a", writePoint(item.a)}, {"b", writePoint(item.b)}, {"label", writePoint(item.label)},
            {"text", item.text}});
    }
    const QJsonObject root{{"format", "Graph3D Drawing"}, {"version", 1}, {"unit", "mm"},
        {"paper", draft.a3 ? "A3" : "A4"}, {"title", draft.title}, {"number", draft.number},
        {"author", draft.author}, {"items", items}};
    writeFile(path, QJsonDocument(root).toJson());
}

}  // namespace graph3d::io
