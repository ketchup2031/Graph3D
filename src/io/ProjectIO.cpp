#include "io/ProjectIO.h"

#include "io/FileIO.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>

#include <QCoreApplication>
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
constexpr int assemblyVersion = 4;
constexpr int sketchVersion = 5;
constexpr int advancedVersion = 6;
constexpr int maxObjects = 10000;
constexpr int maxEntities = 10000;
constexpr auto format = "Graph3D";
constexpr std::array planes{"XY", "XZ", "YZ"};
constexpr std::array axes{"U", "V"};
constexpr std::array modelAxes{"X", "Y", "Z"};
constexpr std::array primitives{"cube", "cylinder", "sphere"};
constexpr std::array operations{"union", "difference", "intersection"};
constexpr std::array treatments{"chamfer", "fillet"};
constexpr std::array constraints{"coincident", "horizontal", "vertical", "perpendicular", "length", "diameter", "projection"};

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
    fileError(QCoreApplication::translate("Diagnostic", "Invalid value for %1.").arg(QLatin1String(field)));
}

double number(const QJsonValue& value, const char* field)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
        fileError(QCoreApplication::translate("Diagnostic", "Expected a finite number for %1.").arg(QLatin1String(field)));
    }
    return value.toDouble();
}

int integer(const QJsonValue& value, const char* field)
{
    const auto parsed = number(value, field);
    if (parsed < 1 || parsed > maxObjects || std::floor(parsed) != parsed) {
        fileError(QCoreApplication::translate("Diagnostic", "Invalid integer for %1.").arg(QLatin1String(field)));
    }
    return static_cast<int>(parsed);
}

int signedInteger(const QJsonValue& value, const char* field)
{
    const auto parsed = value.toInteger(std::numeric_limits<qint64>::min());
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        fileError(QCoreApplication::translate("Diagnostic", "Invalid integer for %1.").arg(QLatin1String(field)));
    }
    return static_cast<int>(parsed);
}

QString string(const QJsonValue& value, const char* field)
{
    if (!value.isString() || value.toString().isEmpty()) {
        fileError(QCoreApplication::translate("Diagnostic", "Missing %1.").arg(QLatin1String(field)));
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

QJsonObject encodeFace(const geometry::FaceReference& face)
{
    return {{"point", encodePoint3(face.center)}, {"normal", encodePoint3(face.outward)}};
}

geometry::FaceReference decodeFace(const QJsonValue& value)
{
    const auto object = value.toObject();
    return {decodePoint3(object.value("point")), decodePoint3(object.value("normal"))};
}

QJsonObject encodeEdge(const geometry::EdgeReference& edge)
{
    QJsonArray samples;
    for (const auto& point : edge.samples) {
        samples.append(encodePoint3(point));
    }
    return {{"curveType", edge.curveType}, {"samples", samples}};
}

geometry::EdgeReference decodeEdge(const QJsonValue& value)
{
    const auto object = value.toObject();
    const auto samples = object.value("samples").toArray();
    geometry::EdgeReference edge{};
    edge.curveType = signedInteger(object.value("curveType"), "curveType");
    if (edge.curveType < 0 || edge.curveType > 8 || samples.size() != static_cast<qsizetype>(edge.samples.size())) {
        fileError(QStringLiteral("Invalid edge reference."));
    }
    for (std::size_t i = 0; i < edge.samples.size(); ++i) {
        edge.samples[i] = decodePoint3(samples.at(static_cast<qsizetype>(i)));
    }
    return edge;
}

template<class T, class Encoder>
QJsonArray encodeList(const std::vector<T>& values, Encoder encode)
{
    QJsonArray result;
    for (const auto& value : values) {
        result.append(encode(value));
    }
    return result;
}

template<class Decoder>
auto decodeList(const QJsonValue& value, qsizetype min, qsizetype max, Decoder decode)
{
    const auto array = value.toArray();
    if (!value.isArray() || array.size() < min || array.size() > max) {
        fileError(QStringLiteral("Invalid advanced operation inputs."));
    }
    std::vector<decltype(decode(QJsonValue{}))> result;
    result.reserve(static_cast<std::size_t>(array.size()));
    for (const auto element : array) {
        result.push_back(decode(element));
    }
    return result;
}

QJsonArray encodeSketchRef(geometry::SketchRef ref)
{
    return {ref.entity, ref.part};
}

geometry::SketchRef decodeSketchRef(const QJsonValue& value)
{
    const auto array = value.toArray();
    if (array.size() != 2) {
        fileError(QStringLiteral("Invalid sketch reference."));
    }
    return {signedInteger(array[0], "entity"), signedInteger(array[1], "part")};
}

QJsonObject encodeConstraint(const geometry::SketchConstraint& constraint)
{
    QJsonObject result{{"kind", name(constraint.kind, constraints)}, {"first", encodeSketchRef(constraint.first)},
        {"second", encodeSketchRef(constraint.second)}, {"value", encodeNumber(constraint.value)}};
    if (constraint.edge) {
        QJsonArray samples;
        for (const auto& point : constraint.edge->samples) {
            samples.append(encodePoint3(point));
        }
        result.insert("edge", QJsonObject{{"curveType", constraint.edge->curveType}, {"samples", samples}});
    }
    return result;
}

geometry::SketchConstraint decodeConstraint(const QJsonValue& value)
{
    const auto object = value.toObject();
    geometry::SketchConstraint result{
        parseName<geometry::ConstraintKind>(object.value("kind"), constraints, "constraint"),
        decodeSketchRef(object.value("first")), decodeSketchRef(object.value("second")),
        number(object.value("value"), "value")};
    if (object.contains("edge")) {
        const auto encoded = object.value("edge").toObject();
        const auto samples = encoded.value("samples").toArray();
        geometry::EdgeReference edge{};
        edge.curveType = signedInteger(encoded.value("curveType"), "curveType");
        if (samples.size() != static_cast<qsizetype>(edge.samples.size()) || edge.curveType < 0 || edge.curveType > 8) {
            fileError(QStringLiteral("Invalid projected sketch edge reference."));
        }
        for (qsizetype i = 0; i < samples.size(); ++i) {
            edge.samples[i] = decodePoint3(samples[i]);
        }
        result.edge = edge;
    }
    return result;
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
            QJsonObject sketch{{"type", "sketch"}, {"plane", name(item.plane, planes)}, {"entities", entities}};
            if (item.datumPlane) {
                sketch.insert("datumPlane", item.datumPlane);
            }
            if (item.support) {
                sketch.insert("support", QJsonObject{{"body", item.support->body},
                    {"point", encodePoint3(item.support->face.center)}, {"normal", encodePoint3(item.support->face.outward)}});
            }
            if (!item.constraints.empty()) {
                QJsonArray relations;
                for (const auto& constraint : item.constraints) {
                    relations.append(encodeConstraint(constraint));
                }
                sketch.insert("constraints", relations);
            }
            return sketch;
        } else if constexpr (std::is_same_v<Type, document::Extrude>) {
            return {{"type", "extrude"}, {"profile", item.profile}, {"distance", encodeNumber(item.distance)}};
        } else if constexpr (std::is_same_v<Type, document::Revolve>) {
            QJsonObject result{{"type", "revolve"}, {"profile", item.profile}, {"axis", name(item.axis, axes)},
                               {"offset", encodeNumber(item.offset)}, {"angle", encodeNumber(item.angle)}};
            if (item.datumAxis) {
                result.insert("datumAxis", item.datumAxis);
            }
            return result;
        } else if constexpr (std::is_same_v<Type, document::Boolean>) {
            return {{"type", "boolean"}, {"first", item.first}, {"second", item.second},
                    {"operation", name(item.operation, operations)}};
        } else if constexpr (std::is_same_v<Type, document::Hole>) {
            return {{"type", "hole"}, {"body", item.body}, {"center", encodePoint3(item.face.center)},
                    {"outward", encodePoint3(item.face.outward)}, {"diameter", encodeNumber(item.size.diameter)},
                    {"depth", encodeNumber(item.size.depth)}, {"through", item.size.through}};
        } else if constexpr (std::is_same_v<Type, document::EdgeTreatment>) {
            auto result = encodeEdge(item.edge);
            result.insert("type", "edge-treatment");
            result.insert("body", item.body);
            result.insert("kind", name(item.kind, treatments));
            result.insert("size", encodeNumber(item.size));
            if (!item.additionalEdges.empty()) {
                result.insert("additionalEdges", encodeList(item.additionalEdges, encodeEdge));
            }
            return result;
        } else if constexpr (std::is_same_v<Type, geometry::DatumPlane>) {
            return {{"type", "datum-plane"}, {"origin", encodePoint3(item.origin)}, {"normal", encodePoint3(item.normal)}};
        } else if constexpr (std::is_same_v<Type, geometry::DatumAxis>) {
            return {{"type", "datum-axis"}, {"origin", encodePoint3(item.origin)}, {"direction", encodePoint3(item.direction)}};
        } else if constexpr (std::is_same_v<Type, document::Sweep>) {
            return {{"type", "sweep"}, {"profile", item.profile}, {"path", encodeList(item.path, encodePoint3)}};
        } else if constexpr (std::is_same_v<Type, document::Loft>) {
            return {{"type", "loft"}, {"profiles", encodeList(item.profiles, [](int id) { return id; })}, {"ruled", item.ruled}};
        } else if constexpr (std::is_same_v<Type, document::Shell>) {
            return {{"type", "shell"}, {"body", item.body}, {"openings", encodeList(item.openings, encodeFace)},
                    {"thickness", encodeNumber(item.thickness)}};
        } else if constexpr (std::is_same_v<Type, document::DraftAngle>) {
            return {{"type", "draft-angle"}, {"body", item.body}, {"neutral", encodeFace(item.neutral)},
                    {"walls", encodeList(item.walls, encodeFace)}, {"angle", encodeNumber(item.degrees)}};
        } else if constexpr (std::is_same_v<Type, document::Transform>) {
            return encodeTransform(item);
        } else if constexpr (std::is_same_v<Type, document::Assembly>) {
            QJsonArray components;
            for (const auto& component : item.components) {
                components.append(QJsonObject{{"body", component.body}, {"anchor", component.placement.anchor},
                    {"offset", encodePoint3(component.placement.offset)}, {"angles", encodePoint3(component.placement.angles)}});
            }
            return {{"type", "assembly"}, {"components", components}};
        } else {
            return {{"type", "imported"}, {"name", QString::fromStdString(item.name)},
                    {"brep", encodeBrep(item.sourceShape)}};
        }
    }, object.parameters);
    data.insert("id", object.id);
    data.insert("visible", object.visible);
    if (object.densityKgM3) {
        data.insert("densityKgM3", encodeNumber(*object.densityKgM3));
    }
    return data;
}

document::Parameters decodeObject(const QJsonObject& object, int version)
{
    const auto type = object.value("type").toString();
    if (version < advancedVersion && (object.contains("datumPlane") || object.contains("datumAxis")
        || object.contains("additionalEdges") || object.contains("densityKgM3"))) {
        fileError(QStringLiteral("This model requires project format version 6."));
    }
    if (version >= advancedVersion) {
        if (type == "datum-plane") {
            return geometry::DatumPlane{decodePoint3(object.value("origin")), decodePoint3(object.value("normal"))};
        }
        if (type == "datum-axis") {
            return geometry::DatumAxis{decodePoint3(object.value("origin")), decodePoint3(object.value("direction"))};
        }
        if (type == "sweep") {
            return document::Sweep{integer(object.value("profile"), "profile"),
                                   decodeList(object.value("path"), 2, 128, decodePoint3)};
        }
        if (type == "loft") {
            if (!object.value("ruled").isBool()) {
                fileError(QStringLiteral("Invalid loft mode."));
            }
            return document::Loft{decodeList(object.value("profiles"), 2, 32,
                [](const auto& value) { return integer(value, "profile"); }), object.value("ruled").toBool()};
        }
        if (type == "shell") {
            return document::Shell{integer(object.value("body"), "body"), decodeList(object.value("openings"), 1, 64, decodeFace),
                                   number(object.value("thickness"), "thickness")};
        }
        if (type == "draft-angle") {
            return document::DraftAngle{integer(object.value("body"), "body"), decodeFace(object.value("neutral")),
                decodeList(object.value("walls"), 1, 64, decodeFace), number(object.value("angle"), "angle")};
        }
    }
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
        if (version < sketchVersion && (object.contains("support") || object.contains("constraints"))) {
            fileError(QStringLiteral("This sketch requires project format version 5."));
        }
        if (version >= sketchVersion) {
            if (object.contains("support")) {
                const auto support = object.value("support").toObject();
                sketch.support = geometry::SketchSupport{integer(support.value("body"), "body"),
                    {decodePoint3(support.value("point")), decodePoint3(support.value("normal"))}};
            }
            if (object.contains("constraints")) {
                const auto relations = object.value("constraints").toArray();
                if (!object.value("constraints").isArray() || relations.size() > 2000) {
                    fileError(QStringLiteral("Invalid sketch constraints."));
                }
                for (const auto relation : relations) {
                    sketch.constraints.push_back(decodeConstraint(relation));
                }
            }
        }
        if (object.contains("datumPlane")) {
            sketch.datumPlane = integer(object.value("datumPlane"), "datumPlane");
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
                                 number(object.value("offset"), "offset"), number(object.value("angle"), "angle"),
                                 object.contains("datumAxis") ? integer(object.value("datumAxis"), "datumAxis") : 0};
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
        auto result = document::EdgeTreatment{integer(object.value("body"), "body"), decodeEdge(object),
            parseName<geometry::EdgeTreatmentKind>(object.value("kind"), treatments, "kind"),
            number(object.value("size"), "size")};
        if (object.contains("additionalEdges")) {
            result.additionalEdges = decodeList(object.value("additionalEdges"), 1, 63, decodeEdge);
        }
        return result;
    }
    if (version >= transformVersion && (type == "linear-array" || type == "polar-array")) {
        const auto body = integer(object.value("body"), "body");
        const auto axis = parseName<geometry::ModelAxis>(object.value("axis"), modelAxes, "axis");
        const auto instances = signedInteger(object.value("count"), "count");
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
    if (type == "assembly" && version >= assemblyVersion) {
        const auto components = object.value("components").toArray();
        if (!object.value("components").isArray() || components.size() < geometry::minAssemblyComponents
            || components.size() > geometry::maxAssemblyComponents) {
            fileError(QStringLiteral("Invalid assembly component list."));
        }
        document::Assembly assembly;
        assembly.components.reserve(static_cast<std::size_t>(components.size()));
        for (const auto entry : components) {
            const auto component = entry.toObject();
            assembly.components.push_back({integer(component.value("body"), "body"),
                {signedInteger(component.value("anchor"), "anchor"), decodePoint3(component.value("offset")),
                 decodePoint3(component.value("angles"))}});
        }
        return assembly;
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
            const auto* sketch = std::get_if<geometry::Sketch>(&object.parameters);
            const auto* revolution = std::get_if<document::Revolve>(&object.parameters);
            const auto* edges = std::get_if<document::EdgeTreatment>(&object.parameters);
            if (document::isAdvanced(object.parameters) || object.densityKgM3 || (sketch && sketch->datumPlane)
                || (revolution && revolution->datumAxis) || (edges && !edges->additionalEdges.empty())) {
                version = advancedVersion;
            } else if (version < sketchVersion && sketch && (sketch->support || !sketch->constraints.empty())) {
                version = sketchVersion;
            } else if (version < assemblyVersion && std::holds_alternative<document::Assembly>(object.parameters)) {
                version = assemblyVersion;
            } else if (version < transformVersion && std::holds_alternative<document::Transform>(object.parameters)) {
                version = transformVersion;
            } else if (version == legacyVersion && (std::holds_alternative<document::Hole>(object.parameters)
                || std::holds_alternative<document::EdgeTreatment>(object.parameters))) {
                version = featureVersion;
            }
            encoded.append(encodeObject(object));
        }
    } catch (const Standard_Failure& error) {
        fileError(QCoreApplication::translate("Diagnostic", "Cannot encode project geometry: %1").arg(QString::fromUtf8(error.GetMessageString())));
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
    return decodeProject(readFile(path));
}

document::Document decodeProject(const QByteArray& data)
{
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) {
        fileError(QCoreApplication::translate("Diagnostic", "Invalid project JSON: %1").arg(error.errorString()));
    }
    const auto root = json.object();
    if (root.value("format").toString() != QLatin1String(format)) {
        fileError(QStringLiteral("This is not a Graph3D project."));
    }
    const auto parsedVersion = root.value("version").toDouble();
    if ((parsedVersion != legacyVersion && parsedVersion != featureVersion
         && parsedVersion != transformVersion && parsedVersion != assemblyVersion && parsedVersion != sketchVersion
         && parsedVersion != advancedVersion)
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
            const auto density = object.contains("densityKgM3")
                ? std::optional(number(object.value("densityKgM3"), "densityKgM3")) : std::nullopt;
            definitions.push_back({decodeObject(object, version), object.value("visible").toBool(), density});
        }
        return document::Document::fromDefinitions(std::move(definitions));
    } catch (const Standard_Failure& failure) {
        fileError(QCoreApplication::translate("Diagnostic", "Cannot read project geometry: %1").arg(QString::fromUtf8(failure.GetMessageString())));
    }
}

}  // namespace graph3d::io
