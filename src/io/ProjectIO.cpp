#include "io/ProjectIO.h"

#include "io/FileIO.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
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
constexpr int linkedVersion = 7;
constexpr int curveSketchVersion = 8;
constexpr int configurationVersion = 9;
constexpr int machiningVersion = 10;
constexpr int motionVersion = 11;
constexpr int fastenerVersion = 12;
constexpr int sheetMetalVersion = 13;
constexpr int maxObjects = 10000;
constexpr int maxEntities = 10000;
constexpr auto format = "Graph3D";
constexpr std::array planes{"XY", "XZ", "YZ"};
constexpr std::array axes{"U", "V"};
constexpr std::array modelAxes{"X", "Y", "Z"};
constexpr std::array primitives{"cube", "cylinder", "sphere"};
constexpr std::array operations{"union", "difference", "intersection"};
constexpr std::array treatments{"chamfer", "fillet"};
constexpr std::array holeStyles{"straight", "counterbore", "countersink"};
constexpr std::array mateModes{"align", "distance", "angle", "travel", "screw"};
constexpr std::array fastenerKinds{"bolt", "nut", "washer"};
constexpr std::array constraints{"coincident", "horizontal", "vertical", "perpendicular", "length", "diameter", "projection",
    "tangent", "parallel", "equal", "symmetric", "angle", "fix-point"};

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
        } else if constexpr (std::is_same_v<Type, geometry::Circle>) {
            return {{"type", "circle"}, {"center", encodePoint(item.center)}, {"radius", encodeNumber(item.radius)}};
        } else if constexpr (std::is_same_v<Type, geometry::Arc>) {
            return {{"type", "arc"}, {"start", encodePoint(item.start)}, {"through", encodePoint(item.through)},
                    {"end", encodePoint(item.end)}};
        } else {
            QJsonArray points;
            for (const auto point : item.points) {
                points.append(encodePoint(point));
            }
            return {{"type", "spline"}, {"points", points}, {"closed", item.closed}};
        }
    }, entity);
}

geometry::SketchEntity decodeEntity(const QJsonValue& value, int version)
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
    if (version >= curveSketchVersion && type == "arc") {
        return geometry::Arc{decodePoint(entity.value("start")), decodePoint(entity.value("through")), decodePoint(entity.value("end"))};
    }
    if (version >= curveSketchVersion && type == "spline") {
        const auto points = entity.value("points").toArray();
        if (points.size() < 2 || points.size() > 128 || !entity.value("closed").isBool()) {
            fileError(QStringLiteral("Invalid sketch spline."));
        }
        geometry::Spline spline{{}, entity.value("closed").toBool()};
        for (const auto point : points) {
            spline.points.push_back(decodePoint(point));
        }
        return spline;
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
    if (constraint.kind == geometry::ConstraintKind::Symmetric) {
        result.insert("axis", encodeSketchRef(constraint.axis));
    }
    if (constraint.kind == geometry::ConstraintKind::FixPoint) {
        result.insert("fixed", encodePoint(constraint.fixed));
    }
    if (constraint.edge) {
        QJsonArray samples;
        for (const auto& point : constraint.edge->samples) {
            samples.append(encodePoint3(point));
        }
        result.insert("edge", QJsonObject{{"curveType", constraint.edge->curveType}, {"samples", samples}});
    }
    return result;
}

geometry::SketchConstraint decodeConstraint(const QJsonValue& value, int version)
{
    const auto object = value.toObject();
    geometry::SketchConstraint result{
        parseName<geometry::ConstraintKind>(object.value("kind"), constraints, "constraint"),
        decodeSketchRef(object.value("first")), decodeSketchRef(object.value("second")),
        number(object.value("value"), "value")};
    if (version < curveSketchVersion && result.kind > geometry::ConstraintKind::OnProjection) {
        fileError(QStringLiteral("This sketch requires project format version 8."));
    }
    if (result.kind == geometry::ConstraintKind::Symmetric) {
        result.axis = decodeSketchRef(object.value("axis"));
    }
    if (result.kind == geometry::ConstraintKind::FixPoint) {
        result.fixed = decodePoint(object.value("fixed"));
    }
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

QJsonObject encodeProperties(const document::PartProperties& properties)
{
    QJsonObject result{{"designation", QString::fromStdString(properties.designation)}, {"name", QString::fromStdString(properties.name)},
            {"material", QString::fromStdString(properties.material)}, {"note", QString::fromStdString(properties.note)}};
    if (properties.color) { result.insert("color", encodePoint3(*properties.color)); }
    return result;
}

document::PartProperties decodeProperties(const QJsonObject& object)
{
    const auto read = [&](const char* field) {
        const auto value = object.value(field);
        if (!value.isString() || value.toString().size() > 1000) {
            fileError(QStringLiteral("Invalid component properties."));
        }
        return value.toString().toStdString();
    };
    document::PartProperties result{read("designation"), read("name"), read("material"), read("note")};
    if (object.contains("color")) {
        const auto rgb = decodePoint3(object.value("color"));
        if (std::ranges::any_of(rgb, [](double v) { return v < 0 || v > 1; })) { fileError(QStringLiteral("Invalid component color.")); }
        result.color = rgb;
    }
    return result;
}

QJsonObject encodeRecord(const document::ComponentRecord& record, int depth, int& remaining)
{
    if (depth > 24 || --remaining < 0 || record.children.size() > geometry::maxAssemblyComponents) {
        fileError(QStringLiteral("Invalid or oversized linked component hierarchy."));
    }
    QJsonArray children;
    for (const auto& child : record.children) {
        children.append(encodeRecord(child, depth + 1, remaining));
    }
    QJsonObject value{{"properties", encodeProperties(record.properties)}, {"children", children}};
    if (record.standard) { value.insert("standard", true); }
    if (!record.identity.empty()) {
        value.insert("identity", QString::fromStdString(record.identity));
    }
    if (record.densityKgM3) {
        value.insert("densityKgM3", *record.densityKgM3);
    }
    return value;
}

document::ComponentRecord decodeRecord(const QJsonValue& value, int depth, int& remaining)
{
    if (depth > 24 || --remaining < 0 || !value.isObject()) {
        fileError(QStringLiteral("Invalid or oversized linked component hierarchy."));
    }
    const auto object = value.toObject();
    document::ComponentRecord result{decodeProperties(object.value("properties").toObject())};
    if (object.contains("standard")) {
        if (!object.value("standard").isBool()) { fileError(QStringLiteral("Invalid catalog component properties.")); }
        result.standard = object.value("standard").toBool();
    }
    if (object.contains("identity")) {
        const auto identity = object.value("identity");
        if (!identity.isString() || identity.toString().size() > 32780 || identity.toString().contains(QChar::Null)) {
            fileError(QStringLiteral("Invalid component properties."));
        }
        result.identity = identity.toString().toStdString();
    }
    if (object.contains("densityKgM3")) {
        const auto density = number(object.value("densityKgM3"), "densityKgM3");
        if (density <= 0 || density > 1.e9) {
            fileError(QStringLiteral("Invalid saved material density."));
        }
        result.densityKgM3 = density;
    }
    const auto children = object.value("children").toArray();
    if (!object.value("children").isArray() || children.size() > geometry::maxAssemblyComponents) {
        fileError(QStringLiteral("Invalid or oversized linked component hierarchy."));
    }
    for (const auto child : children) {
        result.children.push_back(decodeRecord(child, depth + 1, remaining));
    }
    return result;
}

QJsonObject encodeSurface(const geometry::MateSurface& surface)
{
    return {{"kind", surface.kind == geometry::MateKind::Plane ? "plane" : "cylinder"},
            {"point", encodePoint3(surface.point)}, {"direction", encodePoint3(surface.direction)},
            {"radius", encodeNumber(surface.radius)}};
}

geometry::MateSurface decodeSurface(const QJsonValue& value)
{
    const auto object = value.toObject();
    const auto kind = object.value("kind").toString();
    const auto direction = decodePoint3(object.value("direction"));
    const double radius = number(object.value("radius"), "radius");
    if ((kind != "plane" && kind != "cylinder")
        || std::abs(std::hypot(direction[0], direction[1], direction[2]) - 1) > 1.e-6
        || (kind == "plane" ? radius != 0 : radius <= 0)) {
        fileError(QStringLiteral("Invalid mating surface."));
    }
    return {kind == "plane" ? geometry::MateKind::Plane : geometry::MateKind::Cylinder,
            decodePoint3(object.value("point")), direction, radius};
}

QJsonObject encodeMate(const geometry::Mate& mate)
{
    QJsonObject value{{"component", mate.component}, {"target", mate.target}, {"moving", encodeSurface(mate.moving)},
            {"reference", encodeSurface(mate.reference)}, {"opposite", mate.opposite}};
    if (mate.mode != geometry::MateMode::Align) {
        value.insert("mode", name(mate.mode, mateModes));
        value.insert("value", encodeNumber(mate.value));
        value.insert("minimum", encodeNumber(mate.minimum));
        value.insert("maximum", encodeNumber(mate.maximum));
    }
    return value;
}

geometry::Mate decodeMate(const QJsonValue& value, int version)
{
    const auto object = value.toObject();
    if (!object.value("opposite").isBool()) {
        fileError(QStringLiteral("Invalid assembly mate."));
    }
    geometry::Mate mate{integer(object.value("component"), "component"), integer(object.value("target"), "target"),
            decodeSurface(object.value("moving")), decodeSurface(object.value("reference")), object.value("opposite").toBool()};
    if (object.contains("mode")) {
        if (version < motionVersion) { fileError(QStringLiteral("This mate requires project format version 11.")); }
        mate.mode = parseName<geometry::MateMode>(object.value("mode"), mateModes, "mode");
        mate.value = number(object.value("value"), "value");
        mate.minimum = number(object.value("minimum"), "minimum");
        mate.maximum = number(object.value("maximum"), "maximum");
    }
    return mate;
}

QJsonObject encodeObject(const document::Object& object, const QString& path)
{
    auto data = std::visit([&path](const auto& item) -> QJsonObject {
        using Type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Type, document::Primitive>) {
            return {{"type", "primitive"}, {"kind", name(item.kind, primitives)}, {"x", encodeNumber(item.x)}, {"y", encodeNumber(item.y)}};
        } else if constexpr (std::is_same_v<Type, geometry::Fastener>) {
            return {{"type", "fastener"}, {"kind", name(item.kind, fastenerKinds)},
                    {"diameter", item.diameter}, {"length", item.length}};
        } else if constexpr (std::is_same_v<Type, geometry::SheetMetal>) {
            return {{"type","sheet-metal"},{"length",encodeNumber(item.length)},{"width",encodeNumber(item.width)},
                {"thickness",encodeNumber(item.thickness)},{"bends",encodeList(item.bends,[](const auto& bend){
                    return QJsonObject{{"left",bend.left},{"flange",bend.flange},{"radius",bend.radius},
                        {"angle",bend.degrees},{"factor",bend.factor},{"relief",bend.relief}};
                })},{"holes",encodeList(item.holes,[](const auto& hole){
                    return QJsonObject{{"panel",hole.panel},{"along",hole.along},{"across",hole.across},{"diameter",hole.diameter}};
                })}};
        } else if constexpr (std::is_same_v<Type, document::ImportedMesh>) {
            return {{"type","mesh"},{"name",QString::fromStdString(item.name)},
                {"vertices",encodeList(item.vertices,encodePoint3)}};
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
            if (item.trajectory) {
                sketch.insert("trajectory", true);
            }
            if (!item.construction.empty()) {
                QJsonArray indices;
                for (int index : item.construction) {
                    indices.append(index);
                }
                sketch.insert("construction", indices);
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
            QJsonObject result{{"type", "hole"}, {"body", item.body}, {"center", encodePoint3(item.face.center)},
                    {"outward", encodePoint3(item.face.outward)}, {"diameter", encodeNumber(item.size.diameter)},
                    {"depth", encodeNumber(item.size.depth)}, {"through", item.size.through}};
            if (item.size.style != geometry::HoleStyle::Straight) {
                result.insert("style", name(item.size.style, holeStyles));
                result.insert("headDiameter", encodeNumber(item.size.headDiameter));
                result.insert("seat", encodeNumber(item.size.seat));
            }
            if (const auto& t = item.size.thread) {
                result.insert("thread", QJsonObject{{"diameter", encodeNumber(t->diameter)}, {"pitch", encodeNumber(t->pitch)},
                    {"length", encodeNumber(t->length)}, {"left", t->leftHanded}});
            }
            return result;
        } else if constexpr (std::is_same_v<Type, document::EdgeTreatment>) {
            auto result = encodeEdge(item.edge);
            result.insert("type", "edge-treatment");
            result.insert("body", item.body);
            result.insert("kind", name(item.kind, treatments));
            result.insert("size", encodeNumber(item.size));
            if (!item.additionalEdges.empty()) {
                result.insert("additionalEdges", encodeList(item.additionalEdges, encodeEdge));
            }
            if (item.followTangency) { result.insert("tangentChain", true); }
            if (item.endRadius) { result.insert("endRadius", encodeNumber(*item.endRadius)); }
            return result;
        } else if constexpr (std::is_same_v<Type, document::FeaturePattern>) {
            QJsonObject result{{"type", "feature-pattern"}, {"seed", item.seed}, {"count", item.count}};
            if (item.path) { result.insert("path", item.path); result.insert("reverse", item.reverse); }
            else { result.insert("step", encodePoint3(item.step)); }
            return result;
        } else if constexpr (std::is_same_v<Type, document::MoveFace>) {
            return {{"type", "move-face"}, {"body", item.body}, {"face", encodeFace(item.face)},
                    {"distance", encodeNumber(item.distance)}};
        } else if constexpr (std::is_same_v<Type, document::RemoveBlend>) {
            return {{"type", "remove-blend"}, {"body", item.body}, {"center", encodePoint3(item.face.center)},
                    {"area", encodeNumber(item.face.area)}, {"surface", item.face.surface}};
        } else if constexpr (std::is_same_v<Type, geometry::DatumPlane>) {
            return {{"type", "datum-plane"}, {"origin", encodePoint3(item.origin)}, {"normal", encodePoint3(item.normal)}};
        } else if constexpr (std::is_same_v<Type, geometry::DatumAxis>) {
            return {{"type", "datum-axis"}, {"origin", encodePoint3(item.origin)}, {"direction", encodePoint3(item.direction)}};
        } else if constexpr (std::is_same_v<Type, document::Sweep>) {
            if (item.pathSketch) {
                return {{"type", "sweep"}, {"profile", item.profile}, {"pathSketch", item.pathSketch},
                        {"reversePath", item.reversePath}};
            }
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
                QJsonObject value{{"body", component.body}, {"anchor", component.placement.anchor},
                    {"offset", encodePoint3(component.placement.offset)}, {"angles", encodePoint3(component.placement.angles)}};
                if (component.fixed) {
                    value.insert("fixed", true);
                }
                if (component.simplified) { value.insert("simplified", true); }
                if (const auto& context = component.context) {
                    value.insert("context", QJsonObject{{"reference", context->reference}, {"face", encodeFace(context->face)},
                        {"targetFace", encodeFace(context->targetFace)}, {"clearance", encodeNumber(context->clearance)}});
                }
                components.append(value);
            }
            QJsonObject result{{"type", "assembly"}, {"components", components}};
            if (!item.mates.empty()) {
                result.insert("mates", encodeList(item.mates, encodeMate));
            }
            if (item.driveAngle != 0) { result.insert("driveAngle", encodeNumber(item.driveAngle)); }
            return result;
        } else if constexpr (std::is_same_v<Type, document::LinkedComponent>) {
            const QFileInfo owner(path), source(QString::fromStdString(item.sourcePath));
            if (owner == source || QDir::cleanPath(owner.absoluteFilePath()) == QDir::cleanPath(source.absoluteFilePath())) {
                fileError(QStringLiteral("An assembly cannot be saved over one of its component sources."));
            }
            const auto storedPath = owner.absoluteDir().relativeFilePath(source.absoluteFilePath());
            int remaining = 3000;
            return {{"type", "linked-component"}, {"source", storedPath}, {"object", item.sourceObject},
                    {"digest", QString::fromStdString(item.digest)}, {"brep", encodeBrep(item.snapshot)},
                    {"record", encodeRecord(item.record, 0, remaining)}};
        } else {
            return {{"type", "imported"}, {"name", QString::fromStdString(item.name)},
                    {"brep", encodeBrep(item.sourceShape)}};
        }
    }, object.parameters);
    data.insert("id", object.id);
    data.insert("visible", object.visible);
    if (object.properties != document::PartProperties{}) {
        data.insert("properties", encodeProperties(object.properties));
    }
    if (object.densityKgM3) {
        data.insert("densityKgM3", encodeNumber(*object.densityKgM3));
    }
    return data;
}

document::Parameters decodeObject(const QJsonObject& object, int version, const QString& path)
{
    const auto type = object.value("type").toString();
    if (version < linkedVersion && (object.contains("properties") || object.contains("mates") || type == "linked-component")) {
        fileError(QStringLiteral("This model requires project format version 7."));
    }
    if (type == "linked-component") {
        const auto source = string(object.value("source"), "source");
        const auto digest = string(object.value("digest"), "digest").toLatin1();
        if (source.contains(QChar::Null) || source.size() > 32767 || digest.size() != 64
            || QByteArray::fromHex(digest).toHex() != digest) {
            fileError(QStringLiteral("Invalid linked component source."));
        }
        const auto base = path.isEmpty() ? QDir::current() : QFileInfo(path).absoluteDir();
        int remaining = 3000;
        return document::LinkedComponent{QDir::cleanPath(base.absoluteFilePath(source)).toStdString(),
            integer(object.value("object"), "object"), digest.toStdString(), decodeBrep(object.value("brep")),
            decodeRecord(object.value("record"), 0, remaining)};
    }
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
            if (object.contains("pathSketch")) {
                if (version < curveSketchVersion || object.contains("path") || !object.value("reversePath").isBool()) {
                    fileError(QStringLiteral("Invalid linked sweep path."));
                }
                return document::Sweep{integer(object.value("profile"), "profile"), {},
                    integer(object.value("pathSketch"), "path sketch"), object.value("reversePath").toBool()};
            }
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
            sketch.entities.push_back(decodeEntity(entry, version));
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
                    sketch.constraints.push_back(decodeConstraint(relation, version));
                }
            }
        }
        if (object.contains("datumPlane")) {
            sketch.datumPlane = integer(object.value("datumPlane"), "datumPlane");
        }
        if (object.contains("construction") || object.contains("trajectory")) {
            if (version < curveSketchVersion
                || (object.contains("trajectory") && !object.value("trajectory").isBool())
                || (object.contains("construction") && !object.value("construction").isArray())) {
                fileError(QStringLiteral("Invalid sketch construction or trajectory settings."));
            }
            sketch.trajectory = object.value("trajectory").toBool();
            const auto construction = object.value("construction").toArray();
            if (construction.size() > entities.size()) {
                fileError(QStringLiteral("Invalid sketch construction or trajectory settings."));
            }
            for (const auto index : construction) {
                sketch.construction.push_back(signedInteger(index, "construction entity"));
            }
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
        auto result = document::Hole{integer(object.value("body"), "body"),
            {decodePoint3(object.value("center")), decodePoint3(object.value("outward"))},
            {number(object.value("diameter"), "diameter"), number(object.value("depth"), "depth"),
             object.value("through").toBool()}};
        if (object.contains("style")) {
            if (version < machiningVersion) { fileError(QStringLiteral("Unsupported hole style.")); }
            result.size.style = parseName<geometry::HoleStyle>(object.value("style"), holeStyles, "style");
            result.size.headDiameter = number(object.value("headDiameter"), "headDiameter");
            result.size.seat = number(object.value("seat"), "seat");
        }
        if (object.contains("thread")) {
            const auto t = object.value("thread").toObject();
            if (version < machiningVersion || !t.value("left").isBool()) { fileError(QStringLiteral("Invalid hole thread.")); }
            result.size.thread = geometry::ThreadSize{number(t.value("diameter"), "diameter"),
                number(t.value("pitch"), "pitch"), number(t.value("length"), "length"), t.value("left").toBool()};
        }
        return result;
    }
    if (type == "edge-treatment" && version >= featureVersion) {
        auto result = document::EdgeTreatment{integer(object.value("body"), "body"), decodeEdge(object),
            parseName<geometry::EdgeTreatmentKind>(object.value("kind"), treatments, "kind"),
            number(object.value("size"), "size")};
        if (object.contains("additionalEdges")) {
            result.additionalEdges = decodeList(object.value("additionalEdges"), 1, 63, decodeEdge);
        }
        if (object.contains("tangentChain")) {
            if (version < machiningVersion || !object.value("tangentChain").isBool()) { fileError(QStringLiteral("Invalid tangent chain flag.")); }
            result.followTangency = object.value("tangentChain").toBool();
        }
        if (object.contains("endRadius")) {
            if (version < machiningVersion) { fileError(QStringLiteral("Unsupported variable radius.")); }
            result.endRadius = number(object.value("endRadius"), "endRadius");
        }
        return result;
    }
    if (version >= machiningVersion && type == "feature-pattern") {
        document::FeaturePattern result{integer(object.value("seed"), "seed"),
            signedInteger(object.value("count"), "count")};
        if (object.contains("path")) {
            if (object.contains("step") || !object.value("reverse").isBool()) { fileError(QStringLiteral("Invalid feature pattern path.")); }
            result.path = integer(object.value("path"), "path");
            result.reverse = object.value("reverse").toBool();
        } else { result.step = decodePoint3(object.value("step")); }
        return result;
    }
    if (version >= machiningVersion && type == "move-face") {
        return document::MoveFace{integer(object.value("body"), "body"), decodeFace(object.value("face")),
            number(object.value("distance"), "distance")};
    }
    if (version >= machiningVersion && type == "remove-blend") {
        return document::RemoveBlend{integer(object.value("body"), "body"),
            {decodePoint3(object.value("center")), number(object.value("area"), "area"),
             signedInteger(object.value("surface"), "surface")}};
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
        if (!object.value("components").isArray() || components.size() < (version>=sheetMetalVersion?1:geometry::minAssemblyComponents)
            || components.size() > geometry::maxAssemblyComponents) {
            fileError(QStringLiteral("Invalid assembly component list."));
        }
        document::Assembly assembly;
        assembly.components.reserve(static_cast<std::size_t>(components.size()));
        for (const auto entry : components) {
            const auto component = entry.toObject();
            if (component.contains("fixed") && (version < linkedVersion || !component.value("fixed").isBool())) {
                fileError(QStringLiteral("Invalid component grounding flag."));
            }
            if (component.contains("simplified") && (version < motionVersion || !component.value("simplified").isBool())) {
                fileError(QStringLiteral("Invalid simplified component flag."));
            }
            document::Component instance{integer(component.value("body"), "body"),
                {signedInteger(component.value("anchor"), "anchor"), decodePoint3(component.value("offset")),
                 decodePoint3(component.value("angles"))}, component.value("fixed").toBool(), component.value("simplified").toBool()};
            if (component.contains("context")) {
                const auto context = component.value("context").toObject();
                if (version < motionVersion || context.isEmpty()) { fileError(QStringLiteral("Invalid contextual component edit.")); }
                const auto index = integer(context.value("reference"), "reference");
                if (index > static_cast<int>(assembly.components.size())) { fileError(QStringLiteral("Invalid contextual component reference.")); }
                instance.context = geometry::ContextFaceEdit{index, decodeFace(context.value("face")),
                    decodeFace(context.value("targetFace")), number(context.value("clearance"), "clearance")};
            }
            assembly.components.push_back(std::move(instance));
        }
        if (object.contains("mates")) {
            assembly.mates = decodeList(object.value("mates"), 0, 512, [version](const auto& mate) { return decodeMate(mate, version); });
        }
        if (object.contains("driveAngle")) {
            if (version < motionVersion) { fileError(QStringLiteral("This motion requires project format version 11.")); }
            assembly.driveAngle = number(object.value("driveAngle"), "driveAngle");
        }
        return assembly;
    }
    if (type == "fastener" && version >= fastenerVersion) {
        geometry::Fastener item{parseName<geometry::FastenerKind>(object.value("kind"), fastenerKinds, "kind"),
            integer(object.value("diameter"), "diameter"), signedInteger(object.value("length"), "length")};
        geometry::fastenerSize(item);
        return item;
    }
    if (type == "sheet-metal" && version >= sheetMetalVersion) {
        return geometry::SheetMetal{number(object.value("length"),"length"),number(object.value("width"),"width"),
            number(object.value("thickness"),"thickness"), decodeList(object.value("bends"),0,24,[](const auto& value) {
                const auto bend = value.toObject();
                if (!bend.value("left").isBool()) { fileError(QStringLiteral("Invalid sheet bend.")); }
                return geometry::SheetBend{bend.value("left").toBool(), number(bend.value("flange"),"flange"),
                    number(bend.value("radius"),"radius"),number(bend.value("angle"),"angle"),number(bend.value("factor"),"factor"),
                    number(bend.value("relief"),"relief")};
            }),decodeList(object.value("holes"),0,128,[](const auto& value) {
                const auto hole = value.toObject();
                return geometry::SheetHole{signedInteger(hole.value("panel"),"panel"),number(hole.value("along"),"along"),
                    number(hole.value("across"),"across"),number(hole.value("diameter"),"diameter")};
            })};
    }
    if (type=="mesh" && version>=sheetMetalVersion) {
        return document::ImportedMesh{string(object.value("name"),"name").toStdString(),
            decodeList(object.value("vertices"),3,1500000,decodePoint3)};
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
            const auto* assembly = std::get_if<document::Assembly>(&object.parameters);
            const auto* sweep = std::get_if<document::Sweep>(&object.parameters);
            const auto* hole = std::get_if<document::Hole>(&object.parameters);
            const bool curvedSketch = sketch && (sketch->trajectory || !sketch->construction.empty()
                || std::ranges::any_of(sketch->entities, [](const auto& e) {
                    return std::holds_alternative<geometry::Arc>(e) || std::holds_alternative<geometry::Spline>(e);
                })
                || std::ranges::any_of(sketch->constraints, [](const auto& c) {
                    return c.kind > geometry::ConstraintKind::OnProjection;
                }));
            if (std::holds_alternative<geometry::SheetMetal>(object.parameters) || std::holds_alternative<document::ImportedMesh>(object.parameters)
                || (assembly && assembly->components.size()==1) || object.properties.color) {
                version = sheetMetalVersion;
            } else if (std::holds_alternative<geometry::Fastener>(object.parameters)) {
                version = std::max(version, fastenerVersion);
            } else if (assembly && (assembly->driveAngle != 0
                || std::ranges::any_of(assembly->mates, [](const auto& mate) { return mate.mode != geometry::MateMode::Align; })
                || std::ranges::any_of(assembly->components, [](const auto& c) { return c.simplified || c.context.has_value(); }))) {
                version = std::max(version, motionVersion);
            } else if ((hole && (hole->size.thread || hole->size.style != geometry::HoleStyle::Straight))
                || (edges && (edges->followTangency || edges->endRadius))
                || std::holds_alternative<document::FeaturePattern>(object.parameters)
                || std::holds_alternative<document::MoveFace>(object.parameters)
                || std::holds_alternative<document::RemoveBlend>(object.parameters)) {
                version = std::max(version, machiningVersion);
            } else if (curvedSketch || (sweep && sweep->pathSketch)) {
                version = std::max(version, curveSketchVersion);
            } else if (std::holds_alternative<document::LinkedComponent>(object.parameters) || object.properties != document::PartProperties{}
                || (assembly && (!assembly->mates.empty() || std::ranges::any_of(assembly->components, &document::Component::fixed)))) {
                version = std::max(version, linkedVersion);
            } else if (version < advancedVersion && (document::isAdvanced(object.parameters) || object.densityKgM3
                || (sketch && sketch->datumPlane) || (revolution && revolution->datumAxis)
                || (edges && !edges->additionalEdges.empty()))) {
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
            encoded.append(encodeObject(object, path));
        }
    } catch (const Standard_Failure& error) {
        fileError(QCoreApplication::translate("Diagnostic", "Cannot encode project geometry: %1").arg(QString::fromUtf8(error.GetMessageString())));
    }
    QJsonObject root{{"format", format}, {"unit", "mm"}, {"objects", encoded}};
    const auto& configuration = document.configuration();
    if (configuration != document::Configuration{}) {
        version = std::max(version, configurationVersion);
        const auto encodeExpressions = [](const document::Expressions& items) {
            QJsonObject output;
            for (const auto& [name, formula] : items) {
                output.insert(QString::fromStdString(name), QString::fromStdString(formula));
            }
            return output;
        };
        QJsonObject variants, bindings;
        for (const auto& [name, values] : configuration.variants) {
            variants.insert(QString::fromStdString(name), encodeExpressions(values));
        }
        for (const auto& [id, values] : configuration.bindings) {
            bindings.insert(QString::number(id), encodeExpressions(values));
        }
        root.insert("configuration", QJsonObject{{"variables", encodeExpressions(configuration.variables)},
            {"variants", variants}, {"active", QString::fromStdString(configuration.activeVariant)}, {"bindings", bindings}});
    }
    root.insert("version", version);
    const auto data = QJsonDocument(root).toJson();
    if (data.size() > fileLimit) {
        fileError(QStringLiteral("Project exceeds the 64 MiB format limit."));
    }
    writeFile(path, data);
}

document::Document openProject(const QString& path)
{
    return decodeProject(readFile(path), path);
}

document::Document decodeProject(const QByteArray& data, const QString& path)
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
         && parsedVersion != advancedVersion && parsedVersion != linkedVersion && parsedVersion != curveSketchVersion
         && parsedVersion != configurationVersion && parsedVersion != machiningVersion && parsedVersion != motionVersion
         && parsedVersion != fastenerVersion && parsedVersion != sheetMetalVersion)
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
            const auto properties = object.contains("properties") ? decodeProperties(object.value("properties").toObject())
                                                                  : document::PartProperties{};
            definitions.push_back({decodeObject(object, version, path), object.value("visible").toBool(), density, properties});
        }
        document::Configuration configuration;
        if (root.contains("configuration")) {
            const auto cfg = root.value("configuration").toObject();
            if (version < configurationVersion || !cfg.value("variables").isObject() || !cfg.value("variants").isObject()
                || !cfg.value("bindings").isObject() || !cfg.value("active").isString()) {
                fileError(QStringLiteral("Invalid model configuration."));
            }
            const auto decodeExpressions = [](const QJsonValue& item) {
                const auto object = item.toObject();
                if (!item.isObject() || object.size() > 10000) { fileError(QStringLiteral("Invalid formula table.")); }
                document::Expressions result;
                for (auto it = object.begin(); it != object.end(); ++it) {
                    const auto formula = it.value().toString();
                    if (formula.isEmpty() || formula.size() > 512) { fileError(QStringLiteral("Invalid formula.")); }
                    result.emplace(it.key().toStdString(), formula.toStdString());
                }
                return result;
            };
            configuration.variables = decodeExpressions(cfg.value("variables"));
            configuration.activeVariant = cfg.value("active").toString().toStdString();
            const auto variants = cfg.value("variants").toObject();
            for (auto it = variants.begin(); it != variants.end(); ++it) {
                configuration.variants.emplace(it.key().toStdString(), decodeExpressions(it.value()));
            }
            const auto bindings = cfg.value("bindings").toObject();
            for (auto it = bindings.begin(); it != bindings.end(); ++it) {
                bool ok = false;
                const int id = it.key().toInt(&ok);
                if (!ok || it.key() != QString::number(id)) { fileError(QStringLiteral("Invalid formula target ID.")); }
                configuration.bindings.emplace(id, decodeExpressions(it.value()));
            }
        }
        return document::Document::fromDefinitions(std::move(definitions), std::move(configuration));
    } catch (const Standard_Failure& failure) {
        fileError(QCoreApplication::translate("Diagnostic", "Cannot read project geometry: %1").arg(QString::fromUtf8(failure.GetMessageString())));
    }
}

}  // namespace graph3d::io
