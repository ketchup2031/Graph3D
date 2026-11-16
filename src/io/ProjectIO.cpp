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
#include <limits>
#include <locale>
#include <sstream>
#include <type_traits>
#include <utility>

namespace graph3d::io {
namespace {

constexpr int legacyVersion = 1;
constexpr int featureVersion = 2;
constexpr int transformVersion = 3;
constexpr int maxObjects = 10000;
constexpr int maxEntities = 10000;
constexpr auto format = "Graph3D";
constexpr std::array planes{"XY", "XZ", "YZ"};
constexpr std::array axes{"U", "V"};
constexpr std::array modelAxes{"X", "Y", "Z"};
constexpr std::array primitives{"cube", "cylinder", "sphere"};
constexpr std::array operations{"union", "difference", "intersection"};
constexpr std::array treatments{"chamfer", "fillet"};

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

QJsonArray encodePoint3(const geometry::Point3& point)
{
    return {encodeNumber(point[0]), encodeNumber(point[1]), encodeNumber(point[2])};
}

geometry::Point3 decodePoint3(const QJsonValue& value)
{
    const auto coordinates = value.toArray();
    if (coordinates.size() != 3) {
        fileError(QStringLiteral("A 3D point or direction needs three coordinates."));
    }
    return {number(coordinates.at(0), "x"), number(coordinates.at(1), "y"), number(coordinates.at(2), "z")};
}

QJsonObject encodeTransform(const document::Transform& item)
{
    auto data = std::visit([](const auto& transform) -> QJsonObject {
        using Type = std::decay_t<decltype(transform)>;
        if constexpr (std::is_same_v<Type, geometry::LinearArray>) {
            return {{"type", "linear-array"}, {"axis", name(transform.axis, modelAxes)},
                    {"count", transform.count}, {"spacing", encodeNumber(transform.spacing)}};
        } else if constexpr (std::is_same_v<Type, geometry::PolarArray>) {
            return {{"type", "polar-array"}, {"axis", name(transform.axis, modelAxes)},
                    {"count", transform.count}, {"sweep", encodeNumber(transform.sweep)}};
        } else {
            return {{"type", "mirror"}, {"plane", name(transform.plane, planes)},
                    {"offset", encodeNumber(transform.offset)}};
        }
    }, item.parameters);
    data.insert("body", item.body);
    return data;
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
        } else if constexpr (std::is_same_v<Type, document::Hole>) {
            return {{"type", "hole"}, {"body", item.body}, {"center", encodePoint3(item.face.center)},
                    {"outward", encodePoint3(item.face.outward)}, {"diameter", encodeNumber(item.size.diameter)},
                    {"depth", encodeNumber(item.size.depth)}, {"through", item.size.through}};
        } else if constexpr (std::is_same_v<Type, document::EdgeTreatment>) {
            QJsonArray samples;
            for (const auto& point : item.edge.samples) {
                samples.append(encodePoint3(point));
            }
            return {{"type", "edge-treatment"}, {"body", item.body}, {"kind", name(item.kind, treatments)},
                    {"size", encodeNumber(item.size)}, {"curveType", item.edge.curveType}, {"samples", samples}};
        } else if constexpr (std::is_same_v<Type, document::Transform>) {
            return encodeTransform(item);
        } else {
            return {{"type", "imported"}, {"name", QString::fromStdString(item.name)},
                    {"brep", encodeBrep(item.sourceShape)}};
        }
    }, object.parameters);
    data.insert("id", object.id);
    data.insert("visible", object.visible);
    return data;
}

document::Parameters decodeObject(const QJsonObject& object, int version)
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
    if (type == "hole" && version >= featureVersion) {
        if (!object.value("through").isBool()) {
            fileError(QStringLiteral("Missing the hole through flag."));
        }
        return document::Hole{integer(object.value("body"), "body"),
            {decodePoint3(object.value("center")), decodePoint3(object.value("outward"))},
            {number(object.value("diameter"), "diameter"), number(object.value("depth"), "depth"),
             object.value("through").toBool()}};
    }
    if (type == "edge-treatment" && version >= featureVersion) {
        const auto samples = object.value("samples").toArray();
        const auto kind = parseName<geometry::EdgeTreatmentKind>(object.value("kind"), treatments, "kind");
        const auto curve = number(object.value("curveType"), "curveType");
        geometry::EdgeReference edge{};
        if (samples.size() != static_cast<qsizetype>(edge.samples.size())
            || curve < 0 || curve > 8 || std::floor(curve) != curve) {
            fileError(QStringLiteral("Invalid edge reference."));
        }
        edge.curveType = static_cast<int>(curve);
        for (std::size_t i = 0; i < edge.samples.size(); ++i) {
            edge.samples[i] = decodePoint3(samples.at(static_cast<qsizetype>(i)));
        }
        return document::EdgeTreatment{integer(object.value("body"), "body"), edge, kind,
                                       number(object.value("size"), "size")};
    }
    if (version >= transformVersion && (type == "linear-array" || type == "polar-array")) {
        const auto body = integer(object.value("body"), "body");
        const auto axis = parseName<geometry::ModelAxis>(object.value("axis"), modelAxes, "axis");
        // An out-of-range instance count is a rebuild error, just like an invalid spacing.
        const auto count = object.value("count").toInteger(std::numeric_limits<qint64>::min());
        if (count < std::numeric_limits<int>::min() || count > std::numeric_limits<int>::max()) {
            fileError(QStringLiteral("Invalid integer for count."));
        }
        const auto instances = static_cast<int>(count);
        if (type == "linear-array") {
            return document::Transform{body, geometry::LinearArray{axis, instances, number(object.value("spacing"), "spacing")}};
        }
        return document::Transform{body, geometry::PolarArray{axis, instances, number(object.value("sweep"), "sweep")}};
    }
    if (type == "mirror" && version >= transformVersion) {
        return document::Transform{integer(object.value("body"), "body"),
            geometry::Mirror{parseName<geometry::Plane>(object.value("plane"), planes, "plane"),
                             number(object.value("offset"), "offset")}};
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
    int version = legacyVersion;
    try {
        for (const auto& object : document.objects()) {
            if (std::holds_alternative<document::Transform>(object.parameters)) {
                version = transformVersion;
            } else if (version == legacyVersion && (std::holds_alternative<document::Hole>(object.parameters)
                || std::holds_alternative<document::EdgeTreatment>(object.parameters))) {
                version = featureVersion;
            }
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
    const auto parsedVersion = root.value("version").toDouble();
    if ((parsedVersion != legacyVersion && parsedVersion != featureVersion && parsedVersion != transformVersion)
        || root.value("unit").toString() != "mm") {
        fileError(QStringLiteral("Unsupported Graph3D project format version or unit."));
    }
    const auto version = static_cast<int>(parsedVersion);
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
            definitions.push_back({decodeObject(object, version), object.value("visible").toBool()});
        }
        return document::Document::fromDefinitions(std::move(definitions));
    } catch (const Standard_Failure& failure) {
        fileError(QStringLiteral("Cannot read project geometry: %1").arg(QString::fromUtf8(failure.GetMessageString())));
    }
}

}  // namespace graph3d::io
