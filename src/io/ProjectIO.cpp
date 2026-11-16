#include "io/ProjectIO.h"

#include "io/FileIO.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <cmath>
#include <locale>
#include <sstream>
#include <type_traits>
#include <utility>

namespace graph3d::io {
namespace {

constexpr int version = 1;
constexpr int maxObjects = 10000;
constexpr int maxEntities = 10000;
constexpr auto format = "Graph3D";
constexpr std::array planes{"XY", "XZ", "YZ"};
constexpr std::array axes{"U", "V"};
constexpr std::array primitives{"cube", "cylinder", "sphere"};
constexpr std::array operations{"union", "difference", "intersection"};

template<class Enum, std::size_t N>
QString name(Enum value, const std::array<const char*, N>& names)
{
    return QString::fromLatin1(names.at(static_cast<std::size_t>(value)));
}

template<class Enum, std::size_t N>
Enum parseName(const QJsonValue& value, const std::array<const char*, N>& names, const char* field)
{
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (value.toString() == QLatin1String(names[i])) {
            return static_cast<Enum>(i);
        }
    }
    fileError(QStringLiteral("Invalid value for %1.").arg(QLatin1String(field)));
}

double number(const QJsonValue& value, const char* field)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
        fileError(QStringLiteral("Expected a finite number for %1.").arg(QLatin1String(field)));
    }
    return value.toDouble();
}

int integer(const QJsonValue& value, const char* field)
{
    const auto parsed = number(value, field);
    if (parsed < 1 || parsed > maxObjects || std::floor(parsed) != parsed) {
        fileError(QStringLiteral("Invalid integer for %1.").arg(QLatin1String(field)));
    }
    return static_cast<int>(parsed);
}

QString string(const QJsonValue& value, const char* field)
{
    if (!value.isString() || value.toString().isEmpty()) {
        fileError(QStringLiteral("Missing %1.").arg(QLatin1String(field)));
    }
    return value.toString();
}

QJsonValue encodeNumber(double value)
{
    if (!std::isfinite(value)) {
        fileError(QStringLiteral("Project parameters must be finite numbers."));
    }
    return value;
}

QJsonArray encodePoint(geometry::Point2 point)
{
    return {encodeNumber(point.u), encodeNumber(point.v)};
}

geometry::Point2 decodePoint(const QJsonValue& value)
{
    const auto coordinates = value.toArray();
    if (coordinates.size() != 2) {
        fileError(QStringLiteral("A sketch point needs two coordinates."));
    }
    return {number(coordinates.at(0), "u"), number(coordinates.at(1), "v")};
}

QJsonObject encodeEntity(const geometry::SketchEntity& entity)
{
    return std::visit([](const auto& item) -> QJsonObject {
        using Type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Type, geometry::Line>) {
            return {{"type", "line"}, {"a", encodePoint(item.start)}, {"b", encodePoint(item.end)}};
        } else if constexpr (std::is_same_v<Type, geometry::Rectangle>) {
            return {{"type", "rectangle"}, {"a", encodePoint(item.first)}, {"b", encodePoint(item.opposite)}};
        } else {
            return {{"type", "circle"}, {"center", encodePoint(item.center)}, {"radius", encodeNumber(item.radius)}};
        }
    }, entity);
}

geometry::SketchEntity decodeEntity(const QJsonValue& value)
{
    const auto entity = value.toObject();
    const auto type = entity.value("type").toString();
    if (type == "line") {
        return geometry::Line{decodePoint(entity.value("a")), decodePoint(entity.value("b"))};
    }
    if (type == "rectangle") {
        return geometry::Rectangle{decodePoint(entity.value("a")), decodePoint(entity.value("b"))};
    }
    if (type == "circle") {
        return geometry::Circle{decodePoint(entity.value("center")), number(entity.value("radius"), "radius")};
    }
    fileError(QStringLiteral("Unknown sketch entity type."));
}

QString encodeBrep(const TopoDS_Shape& shape)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    BRepTools::Write(shape, stream, false, false, TopTools_FormatVersion_VERSION_1);
    if (!stream) {
        fileError(QStringLiteral("Could not encode imported geometry."));
    }
    return QString::fromLatin1(QByteArray::fromStdString(stream.str()).toBase64());
}

TopoDS_Shape decodeBrep(const QJsonValue& value)
{
    const auto encoded = string(value, "brep").toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || (!decoded.decoded.startsWith("DBRep_DrawableShape")
        && !decoded.decoded.trimmed().startsWith("CASCADE Topology"))) {
        fileError(QStringLiteral("Invalid embedded BREP data."));
    }
    std::istringstream stream(decoded.decoded.toStdString());
    stream.imbue(std::locale::classic());
    TopoDS_Shape shape;
    BRepTools::Read(shape, stream, BRep_Builder{});
    if (shape.IsNull()) {
        fileError(QStringLiteral("The embedded BREP geometry could not be read."));
    }
    return shape;
}

QJsonObject encodeObject(const document::Object& object)
{
    auto data = std::visit([](const auto& item) -> QJsonObject {
        using Type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Type, document::Primitive>) {
            return {{"type", "primitive"}, {"kind", name(item.kind, primitives)}, {"x", encodeNumber(item.x)}, {"y", encodeNumber(item.y)}};
        } else if constexpr (std::is_same_v<Type, geometry::Sketch>) {
            if (item.entities.size() > maxEntities) {
                fileError(QStringLiteral("Too many entities in a sketch."));
            }
            QJsonArray entities;
            for (const auto& entity : item.entities) {
                entities.append(encodeEntity(entity));
            }
            return {{"type", "sketch"}, {"plane", name(item.plane, planes)}, {"entities", entities}};
        } else if constexpr (std::is_same_v<Type, document::Extrude>) {
            return {{"type", "extrude"}, {"profile", item.profile}, {"distance", encodeNumber(item.distance)}};
        } else if constexpr (std::is_same_v<Type, document::Revolve>) {
            return {{"type", "revolve"}, {"profile", item.profile}, {"axis", name(item.axis, axes)},
                    {"offset", encodeNumber(item.offset)}, {"angle", encodeNumber(item.angle)}};
        } else if constexpr (std::is_same_v<Type, document::Boolean>) {
            return {{"type", "boolean"}, {"first", item.first}, {"second", item.second},
                    {"operation", name(item.operation, operations)}};
        } else {
            return {{"type", "imported"}, {"name", QString::fromStdString(item.name)},
                    {"brep", encodeBrep(item.sourceShape)}};
        }
    }, object.parameters);
    data.insert("id", object.id);
    data.insert("visible", object.visible);
    return data;
}

document::Parameters decodeObject(const QJsonObject& object)
{
    const auto type = object.value("type").toString();
    if (type == "primitive") {
        return document::Primitive{parseName<geometry::Primitive>(object.value("kind"), primitives, "kind"),
                                   number(object.value("x"), "x"), number(object.value("y"), "y")};
    }
    if (type == "sketch") {
        const auto entities = object.value("entities").toArray();
        if (!object.value("entities").isArray() || entities.size() > maxEntities) {
            fileError(QStringLiteral("Invalid sketch entities."));
        }
        geometry::Sketch sketch{parseName<geometry::Plane>(object.value("plane"), planes, "plane"), {}};
        sketch.entities.reserve(static_cast<std::size_t>(entities.size()));
        for (const auto entry : entities) {
            sketch.entities.push_back(decodeEntity(entry));
        }
        return sketch;
    }
    if (type == "extrude") {
        return document::Extrude{integer(object.value("profile"), "profile"),
                                 number(object.value("distance"), "distance")};
    }
    if (type == "revolve") {
        return document::Revolve{integer(object.value("profile"), "profile"),
                                 parseName<geometry::Axis>(object.value("axis"), axes, "axis"),
                                 number(object.value("offset"), "offset"), number(object.value("angle"), "angle")};
    }
    if (type == "boolean") {
        return document::Boolean{integer(object.value("first"), "first"), integer(object.value("second"), "second"),
            parseName<geometry::BooleanOperation>(object.value("operation"), operations, "operation")};
    }
    if (type == "imported") {
        return document::Imported{string(object.value("name"), "name").toStdString(), decodeBrep(object.value("brep"))};
    }
    fileError(QStringLiteral("Unknown project object type."));
}

}  // namespace

void saveProject(const QString& path, const document::Document& document)
{
    if (document.objects().size() > maxObjects) {
        fileError(QStringLiteral("Too many objects in this project."));
    }
    QJsonArray encoded;
    try {
        for (const auto& object : document.objects()) {
            encoded.append(encodeObject(object));
        }
    } catch (const Standard_Failure& error) {
        fileError(QStringLiteral("Cannot encode project geometry: %1").arg(QString::fromUtf8(error.GetMessageString())));
    }
    const auto data = QJsonDocument(QJsonObject{{"format", format}, {"version", version},
                                              {"unit", "mm"}, {"objects", encoded}}).toJson();
    if (data.size() > fileLimit) {
        fileError(QStringLiteral("Project exceeds the 64 MiB format limit."));
    }
    writeFile(path, data);
}

document::Document openProject(const QString& path)
{
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(readFile(path), &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) {
        fileError(QStringLiteral("Invalid project JSON: %1").arg(error.errorString()));
    }
    const auto root = json.object();
    if (root.value("format").toString() != QLatin1String(format)) {
        fileError(QStringLiteral("This is not a Graph3D project."));
    }
    if (root.value("version").toDouble() != version || root.value("unit").toString() != "mm") {
        fileError(QStringLiteral("Unsupported Graph3D project format version or unit."));
    }
    const auto entries = root.value("objects").toArray();
    if (!root.value("objects").isArray() || entries.size() > maxObjects) {
        fileError(QStringLiteral("Invalid or oversized project object list."));
    }
    std::vector<document::Definition> definitions;
    definitions.reserve(static_cast<std::size_t>(entries.size()));
    try {
        for (const auto entry : entries) {
            const auto object = entry.toObject();
            if (integer(object.value("id"), "id") != static_cast<int>(definitions.size()) + 1
                || !object.value("visible").isBool()) {
                fileError(QStringLiteral("Invalid object ID or visibility flag."));
            }
            definitions.push_back({decodeObject(object), object.value("visible").toBool()});
        }
        return document::Document::fromDefinitions(std::move(definitions));
    } catch (const Standard_Failure& failure) {
        fileError(QStringLiteral("Cannot read project geometry: %1").arg(QString::fromUtf8(failure.GetMessageString())));
    }
}

}  // namespace graph3d::io
