#include "io/ModelViewsIO.h"
#include "geometry/AdvancedModeling.h"
#include "io/AssemblyDrawingIO.h"

#include "io/FileIO.h"
#include "io/ProjectIO.h"
#include "io/SpecificationIO.h"

#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Vec.hxx>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <numbers>

#include <algorithm>
#include <cmath>

namespace graph3d::io {
namespace {

using namespace drawing;

constexpr int maxVertices = 750000;
constexpr double maxCoordinate = 1e9;
constexpr double maxScale = 1000;
constexpr std::array planes{"XY", "XZ", "YZ"};

[[noreturn]] void invalid()
{
    fileError(QCoreApplication::translate("ModelViews", "Invalid saved model views."));
}

double number(const QJsonValue& value, double maximum = maxCoordinate)
{
    const auto result = value.toDouble();
    if (!value.isDouble() || !std::isfinite(result) || std::abs(result) > maximum) {
        invalid();
    }
    return result;
}

double positive(const QJsonValue& value, double maximum = maxCoordinate)
{
    const auto result = number(value, maximum);
    if (result <= 0) {
        invalid();
    }
    return result;
}

QJsonArray array(const QJsonValue& value)
{
    if (!value.isArray()) {
        invalid();
    }
    return value.toArray();
}

Point readPoint(const QJsonValue& value)
{
    const auto coords = array(value);
    if (coords.size() != 2) {
        invalid();
    }
    return {number(coords[0]), number(coords[1])};
}

QJsonArray writePoint(Point point)
{
    return {point.x, point.y};
}

void consume(int& remaining, qsizetype count)
{
    if (count > remaining) {
        fileError(QCoreApplication::translate("ModelViews", "Model views are too large to store in this drawing."));
    }
    remaining -= static_cast<int>(count);
}

Linework readEdges(const QJsonValue& value, int& remaining)
{
    Linework out;
    for (const auto lineValue : array(value)) {
        const auto points = array(lineValue);
        if (points.size() < 2) {
            invalid();
        }
        consume(remaining, points.size());
        std::vector<Point> line;
        line.reserve(points.size());
        for (const auto point : points) {
            line.push_back(readPoint(point));
        }
        out.push_back(std::move(line));
    }
    return out;
}

QJsonArray writeEdges(const Linework& edges)
{
    QJsonArray output;
    for (const auto& line : edges) {
        QJsonArray points;
        for (const auto& point : line) {
            points.append(writePoint(point));
        }
        output.append(points);
    }
    return output;
}

View readView(const QJsonValue& value, int& remaining)
{
    const auto object = value.toObject();
    View view{readEdges(object.value("visible"), remaining), readEdges(object.value("hidden"), remaining),
              positive(object.value("width")), positive(object.value("height")), {}};
    if (view.visible.empty()) {
        invalid();
    }
    const auto circles = array(object.value("circles"));
    consume(remaining, circles.size());
    for (const auto value : circles) {
        const auto circle = value.toObject();
        view.circles.push_back({readPoint(circle.value("center")), positive(circle.value("radius"))});
    }
    if (object.contains("id")) {
        view.id = QUuid(object.value("id").toString());
        if (view.id.isNull()) {
            invalid();
        }
        view.origin = readPoint(object.value("origin"));
        const auto edges = array(object.value("edges"));
        consume(remaining, 2 * edges.size());
        QSet<QUuid> ids;
        for (const auto value : edges) {
            const auto edge = value.toObject();
            const QUuid id(edge.value("id").toString());
            const auto radius = number(edge.value("radius"));
            const auto angle = number(edge.value("angle"), std::numbers::pi + 1.e-6);
            const auto sweep = number(edge.value("sweep"), 2 * std::numbers::pi + 1.e-6);
            if (id.isNull() || ids.contains(id) || radius < 0 || sweep < 0 || (radius > 0) != (sweep > 0)) {
                invalid();
            }
            ids.insert(id);
            view.edges.push_back({id, readPoint(edge.value("a")), readPoint(edge.value("b")), radius, angle, sweep});
        }
    }
    return view;
}

QJsonObject writeView(const View& view, bool references)
{
    QJsonArray circles;
    for (const auto& circle : view.circles) {
        circles.append(QJsonObject{{"center", writePoint(circle.center)}, {"radius", circle.radius}});
    }
    QJsonObject result{{"width", view.width}, {"height", view.height},
                       {"visible", writeEdges(view.visible)}, {"hidden", writeEdges(view.hidden)}, {"circles", circles}};
    if (references && !view.id.isNull()) {
        QJsonArray edges;
        for (const auto& edge : view.edges) {
            edges.append(QJsonObject{{"id", edge.id.toString(QUuid::WithoutBraces)}, {"a", writePoint(edge.a)},
                {"b", writePoint(edge.b)}, {"radius", edge.radius}, {"angle", edge.angle}, {"sweep", edge.sweep}});
        }
        result.insert("id", view.id.toString(QUuid::WithoutBraces));
        result.insert("origin", writePoint(view.origin));
        result.insert("edges", edges);
    }
    return result;
}

Section readSection(const QJsonObject& object, int& remaining)
{
    const auto plane = object.value("plane").toString();
    const auto found = std::ranges::find_if(planes, [&](const char* name) { return plane == QLatin1String(name); });
    if (found == planes.end()) {
        invalid();
    }
    Section section{static_cast<geometry::Plane>(found - planes.begin()), number(object.value("offset")),
                    readView(object.value("view"), remaining), {}};
    for (const auto value : array(object.value("material"))) {
        auto face = readEdges(value, remaining);
        if (face.empty()) {
            invalid();
        }
        section.material.push_back(std::move(face));
    }
    if (section.material.empty()) {
        invalid();
    }
    return section;
}

QJsonObject writeSection(const Section& cut, bool references)
{
    QJsonArray material;
    for (const auto& face : cut.material) {
        material.append(writeEdges(face));
    }
    return {{"plane", planes.at(static_cast<std::size_t>(cut.plane))}, {"offset", cut.offset},
            {"view", writeView(cut.view, references)}, {"material", material}};
}

int validateSnapshot(const ModelProjection& snapshot, const std::vector<DrawingView>& additional = {})
{
    int remaining = maxVertices;
    const auto check = [](double value) {
        if (!std::isfinite(value) || std::abs(value) > maxCoordinate) {
            invalid();
        }
    };
    const auto point = [&](const Point& value) {
        check(value.x);
        check(value.y);
    };
    const auto edges = [&](const Linework& paths) {
        for (const auto& path : paths) {
            if (path.size() < 2) {
                invalid();
            }
            consume(remaining, path.size());
            for (const auto& vertex : path) {
                point(vertex);
            }
        }
    };
    const auto view = [&](const View& value) {
        check(value.width);
        check(value.height);
        if (value.width <= 0 || value.height <= 0 || value.visible.empty()) {
            invalid();
        }
        edges(value.visible);
        edges(value.hidden);
        consume(remaining, value.circles.size() + 2 * value.edges.size());
        for (const auto& circle : value.circles) {
            point(circle.center);
            check(circle.radius);
            if (circle.radius <= 0) {
                invalid();
            }
        }
    };
    view(snapshot.views.front);
    view(snapshot.views.right);
    view(snapshot.views.top);
    if (const auto& cut = snapshot.section) {
        check(cut->offset);
        view(cut->view);
        for (const auto& face : cut->material) {
            edges(face);
        }
    }
    for (const auto& extra : additional) {
        if (!extra.geometry) {
            invalid();
        }
        view(extra.geometry->view);
        for (const auto& face : extra.geometry->material) {
            edges(face);
        }
    }
    return remaining;
}

QJsonArray writeAdditionalViews(const ModelViews& model)
{
    QSet<QUuid> seen;
    for (int i = 0; i < 3; ++i) {
        seen.insert(baseModelView(model, i).id);
    }
    QJsonArray output;
    if (model.additional.size() > drawingViewLimit) {
        invalid();
    }
    for (const auto& entry : model.additional) {
        if (entry.id.isNull() || seen.contains(entry.id) || entry.name.size() > 80
            || (derivedView(entry.kind) && (entry.source.isNull() || !seen.contains(entry.source)))
            || entry.path.size() > 16 || !entry.geometry || entry.geometry->view.id != entry.id) {
            invalid();
        }
        seen.insert(entry.id);
        QJsonArray path, material;
        for (const auto p : entry.path) {
            path.append(writePoint({p.x(), p.y()}));
        }
        for (const auto& face : entry.geometry->material) {
            material.append(writeEdges(face));
        }
        output.append(QJsonObject{{"id", entry.id.toString(QUuid::WithoutBraces)}, {"kind", static_cast<int>(entry.kind)},
            {"source", entry.source.toString(QUuid::WithoutBraces)}, {"name", entry.name},
            {"center", writePoint({entry.placement.center.x(), entry.placement.center.y()})},
            {"scale", entry.placement.scale}, {"yaw", entry.angles[0]}, {"pitch", entry.angles[1]},
            {"roll", entry.angles[2]}, {"path", path}, {"focus", writePoint({entry.focus.x(), entry.focus.y()})},
            {"radius", entry.radius}, {"depth", entry.depth}, {"view", writeView(entry.geometry->view, true)},
            {"material", material}});
    }
    return output;
}

void readAdditionalViews(ModelViews& model, const QJsonValue& value, int& remaining)
{
    const auto entries = array(value);
    if (entries.size() > drawingViewLimit) {
        invalid();
    }
    QSet<QUuid> seen;
    for (int i = 0; i < 3; ++i) {
        const auto id = baseModelView(model, i).id;
        if (id.isNull() || seen.contains(id)) {
            invalid();
        }
        seen.insert(id);
    }
    for (const auto value : entries) {
        const auto entry = value.toObject();
        DrawingView extra;
        extra.id = QUuid(entry.value("id").toString());
        const int kind = entry.value("kind").toInt(-1);
        if (extra.id.isNull() || seen.contains(extra.id) || kind < 0 || kind > static_cast<int>(DrawingViewKind::Local)
            || !entry.value("name").isString() || entry.value("name").toString().size() > 80) {
            invalid();
        }
        extra.kind = static_cast<DrawingViewKind>(kind);
        extra.name = entry.value("name").toString();
        extra.source = QUuid(entry.value("source").toString());
        if (derivedView(extra.kind) && !seen.contains(extra.source)) {
            invalid();
        }
        seen.insert(extra.id);
        const auto center = readPoint(entry.value("center"));
        extra.placement = {{center.x, center.y}, positive(entry.value("scale"), maxScale)};
        extra.angles = {number(entry.value("yaw"), 360), number(entry.value("pitch"), 90), number(entry.value("roll"), 360)};
        const auto focus = readPoint(entry.value("focus"));
        extra.focus = {focus.x, focus.y};
        extra.radius = positive(entry.value("radius"));
        extra.depth = number(entry.value("depth"));
        const auto path = array(entry.value("path"));
        if (path.size() > 16) {
            invalid();
        }
        for (const auto value : path) {
            const auto point = readPoint(value);
            extra.path.emplace_back(point.x, point.y);
        }
        auto geometry = std::make_shared<CutProjection>();
        geometry->view = readView(entry.value("view"), remaining);
        if (geometry->view.id != extra.id) {
            invalid();
        }
        for (const auto face : array(entry.value("material"))) {
            geometry->material.push_back(readEdges(face, remaining));
        }
        extra.geometry = std::move(geometry);
        model.additional.push_back(std::move(extra));
    }
}

QByteArray digest(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

}  // namespace

ModelSource openModelSource(const QString& path)
{
    const auto bytes = readFile(path);
    return {QDir::cleanPath(QFileInfo(path).absoluteFilePath()), digest(bytes), decodeProject(bytes, path)};
}

QByteArray currentModelDigest(const QString& path)
{
    return digest(readFile(path));
}

bool canProjectBody(const document::Object& item)
{
    return item.status == document::Status::Ready && !item.shape.IsNull()
        && document::isBody(item.parameters);
}

ModelViews buildModelViews(const ModelSource& source, int bodyId, std::optional<SectionSettings> cut,
                           QSizeF paper, const ModelViews* previous, std::optional<QString> variant)
{
    if (!variant && previous) { variant = previous->variant; }
    std::optional<ModelSource> configured;
    if (variant && variant->toStdString() != source.document.configuration().activeVariant) {
        configured = source;
        configured->document.selectVariant(variant->toStdString());
    }
    const auto& selected = configured ? *configured : source;
    const auto* item = selected.document.find(bodyId);
    if (!item || !canProjectBody(*item)) {
        fileError(QCoreApplication::translate("ModelViews", "Body #%1 is unavailable in the source model.").arg(bodyId));
    }
    auto snapshot = std::make_shared<ModelProjection>();
    snapshot->views = project(item->shape);
    if (cut) {
        snapshot->section = section(item->shape, cut->plane, cut->offset);
    }
    if (previous && previous->geometry && previous->bodyId == bodyId
        && (previous->sourcePath == source.path || previous->sourceDigest == source.digest)) {
        const auto& old = *previous->geometry;
        retainProjectionReferences(snapshot->views.front, old.views.front);
        retainProjectionReferences(snapshot->views.right, old.views.right);
        retainProjectionReferences(snapshot->views.top, old.views.top);
        if (snapshot->section && old.section && snapshot->section->plane == old.section->plane) {
            retainProjectionReferences(snapshot->section->view, old.section->view);
        }
    }
    if (previous && previous->geometry) {
        // Layout links identify their view even when the source body is replaced.
        const auto preserveId = [](View& slot, const View& old) {
            if (!old.id.isNull()) {
                slot.id = old.id;
            }
        };
        const auto& old = previous->geometry->views;
        preserveId(snapshot->views.front, old.front);
        preserveId(snapshot->views.right, old.right);
        preserveId(snapshot->views.top, old.top);
    }
    auto model = previous ? *previous : ModelViews{};
    model.variant = variant;
    model.sourcePath = source.path;
    model.bodyId = bodyId;
    model.sourceDigest = source.digest;
    model.titleData = {QString::fromStdString(item->properties.name), QString::fromStdString(item->properties.designation), {}};
    try {
        model.titleData.massKg = selected.document.measure(bodyId).massKg;
    } catch (const std::invalid_argument&) {
        // A linked title can still be generated when mass is unavailable.
    }
    if (!previous) {
        model.layout = defaultViewLayout(snapshot->views, paper);
    }
    model.geometry = snapshot;
    model.threads.clear();
    const auto addThread = [&](const document::Hole& hole, const geometry::Point3& offset) {
        if (!hole.size.thread) { return; }
        const auto& thread = *hole.size.thread;
        const auto& c = hole.face.center;
        const auto& n = hole.face.outward;
        const std::array align{-n[1], n[0], n[2]};
        const auto best = std::ranges::max_element(align, {}, [](double value) { return std::abs(value); });
        const int index = static_cast<int>(best - align.begin());
        const auto& view = baseModelView(model, index);
        const auto x = c[0] + offset[0], y = c[1] + offset[1], z = c[2] + offset[2];
        const auto center = index == 0 ? QPointF(x, z) : index == 1 ? QPointF(y, z) : QPointF(x, y);
        if (model.threads.size() >= 256) { fileError(QStringLiteral("Too many drawing thread callouts.")); }
        model.threads.push_back({index, center - QPointF(view.origin.x, view.origin.y), thread.diameter,
            QStringLiteral("M%1×%2%3 / %4 mm").arg(thread.diameter, 0, 'g', 6).arg(thread.pitch, 0, 'g', 6)
                .arg(thread.leftHanded ? QStringLiteral(" LH") : QString{}).arg(thread.length, 0, 'g', 6)});
    };
    for (int id = bodyId; id > 0;) {
        const auto params = selected.document.effectiveParameters(id);
        if (const auto* pattern = std::get_if<document::FeaturePattern>(&params)) {
            const auto seed = selected.document.effectiveParameters(pattern->seed);
            if (const auto* hole = std::get_if<document::Hole>(&seed)) {
                if (pattern->path) {
                    const auto* path = selected.document.find(pattern->path);
                    for (const auto& offset : geometry::pathArrayOffsets(TopoDS::Wire(path->shape), pattern->count, pattern->reverse)) {
                        addThread(*hole, offset);
                    }
                } else {
                    for (int i = 1; i < pattern->count; ++i) {
                        addThread(*hole, {i*pattern->step[0], i*pattern->step[1], i*pattern->step[2]});
                    }
                }
            }
            id = pattern->seed;
        } else {
            if (const auto* hole = std::get_if<document::Hole>(&params)) { addThread(*hole, {}); }
            id = document::featureBase(params);
        }
    }
    const bool sameSource = previous && previous->bodyId == bodyId
        && (previous->sourcePath == source.path || previous->sourceDigest == source.digest);
    if (!sameSource) { model.annotationSelection.clear(); }
    model.annotations.clear();
    for (const auto& request : model.annotationSelection) {
        if (request.view < 0 || request.view > 2 || !selected.document.annotationBelongsTo(request.sourceId, bodyId)) { continue; }
        const auto annotation = selected.document.resolveAnnotation(request.sourceId);
        if (!annotation) { continue; }
        const auto& view = baseModelView(model, request.view);
        const auto direction = standardDirection(request.view);
        const gp_Vec normal(direction.normal[0], direction.normal[1], direction.normal[2]);
        const gp_Vec x(direction.right[0], direction.right[1], direction.right[2]);
        const auto y = normal.Crossed(x);
        const auto toView = [&](const geometry::Point3& point) {
            const gp_Vec position(point[0], point[1], point[2]);
            return QPointF(position.Dot(x) - view.origin.x, position.Dot(y) - view.origin.y);
        };
        const gp_Vec face(annotation->normal[0], annotation->normal[1], annotation->normal[2]);
        model.annotations.push_back({request.sourceId, request.view, static_cast<ModelAnnotationKind>(annotation->kind),
            toView(annotation->first), toView(annotation->second), annotation->value,
            QString::fromStdString(annotation->text), std::abs(face.Dot(normal)) > .999});
    }
    rebuildDrawingViews(model, item->shape, sameSource ? previous : nullptr);
    validateSnapshot(*snapshot, model.additional);
    const auto prior = previous && previous->bodyId == bodyId && previous->sourcePath == source.path
        ? previous->assembly.get() : nullptr;
    model.assembly = buildAssemblyDrawing(selected, *item, prior);
    if (!sameSource || !model.assembly) { model.specificationPath.clear(); }
    if (!model.specificationPath.isEmpty() && previous->variant == model.variant) { attachSpecification(model, model.specificationPath); }
    else { model.specificationPath.clear(); }
    return model;
}

ModelViews readModelViews(const QJsonObject& object, const QString& draftPath)
{
    const auto path = object.value("source").toString();
    const auto hash = object.value("digest").toString().toLatin1();
    const auto bodyId = positive(object.value("body"), 10000);
    const auto layout = array(object.value("layout"));
    if (path.isEmpty() || path.contains(QChar::Null) || hash.size() != 64
        || QByteArray::fromHex(hash).toHex() != hash || std::floor(bodyId) != bodyId
        || layout.size() != 3 || !object.value("dimensions").isBool() || !object.value("hiddenLines").isBool()) {
        invalid();
    }
    ModelViews result;
    result.sourcePath = QDir::cleanPath(QFileInfo(draftPath).absoluteDir().absoluteFilePath(path));
    result.sourceDigest = QByteArray::fromHex(hash);
    result.bodyId = static_cast<int>(bodyId);
    if (object.contains("specification")) {
        const auto path = object.value("specification").toString();
        if (path.isEmpty() || path.size() > 32768 || path.contains(QChar::Null)) { invalid(); }
        result.specificationPath = QDir::cleanPath(QFileInfo(draftPath).absoluteDir().absoluteFilePath(path));
    }
    if (object.contains("variant")) {
        const auto variant = object.value("variant");
        if (!variant.isString() || variant.toString().toUtf8().size() > 128 || variant.toString().contains(QChar::Null)) {
            invalid();
        }
        result.variant = variant.toString();
    }
    result.dimensions = object.value("dimensions").toBool();
    result.hiddenLines = object.value("hiddenLines").toBool();
    for (int i = 0; i < 3; ++i) {
        const auto entry = layout[i].toObject();
        const auto center = readPoint(entry.value("center"));
        result.layout[i] = {{center.x, center.y}, positive(entry.value("scale"), maxScale)};
    }
    int remaining = maxVertices;
    auto snapshot = std::make_shared<ModelProjection>();
    snapshot->views = {readView(object.value("front"), remaining), readView(object.value("top"), remaining),
                       readView(object.value("right"), remaining)};
    if (object.contains("section")) {
        snapshot->section = readSection(object.value("section").toObject(), remaining);
    }
    if (object.contains("detail")) {
        const auto entry = object.value("detail").toObject();
        const int source = entry.value("view").toInt(-1);
        if (source < 0 || source > 2) {
            invalid();
        }
        const auto focus = readPoint(entry.value("focus"));
        const auto center = readPoint(entry.value("center"));
        result.detail = drawing::DetailView{source, {focus.x, focus.y}, positive(entry.value("radius"), maxCoordinate),
            {{center.x, center.y}, positive(entry.value("scale"), maxScale)}};
    }
    if (object.contains("local")) {
        if (!snapshot->section) {
            invalid();
        }
        const auto entry = object.value("local").toObject();
        const auto focus = readPoint(entry.value("focus"));
        result.local = drawing::LocalSection{{focus.x, focus.y}, positive(entry.value("radius"), maxCoordinate)};
    }
    result.geometry = std::move(snapshot);
    if (object.contains("baseVisible")) {
        const auto visibility = array(object.value("baseVisible"));
        if (visibility.size() != 3) {
            invalid();
        }
        for (int i = 0; i < 3; ++i) {
            if (!visibility[i].isBool()) {
                invalid();
            }
            result.showBase[i] = visibility[i].toBool();
        }
    }
    if (object.contains("additional")) {
        readAdditionalViews(result, object.value("additional"), remaining);
    }
    if (object.contains("titleData")) {
        const auto title = object.value("titleData").toObject();
        result.titleData.name = title.value("name").toString();
        result.titleData.designation = title.value("designation").toString();
        if (result.titleData.name.size() > 200 || result.titleData.designation.size() > 200) {
            invalid();
        }
        if (title.contains("mass")) {
            result.titleData.massKg = positive(title.value("mass"), 1.e20);
        }
    }
    if (object.contains("assembly")) {
        result.assembly = std::make_shared<AssemblyDrawing>(readAssemblyDrawing(object.value("assembly")));
    }
    if (object.contains("threads")) {
        const auto entries = array(object.value("threads"));
        if (entries.size() > 256) { invalid(); }
        for (const auto& value : entries) {
            const auto entry = value.toObject();
            const int view = entry.value("view").toInt(-1);
            const auto center = readPoint(entry.value("center"));
            const auto designation = entry.value("designation").toString();
            if (view < 0 || view > 2 || designation.isEmpty() || designation.size() > 100) { invalid(); }
            result.threads.push_back({view, {center.x, center.y}, positive(entry.value("diameter"), maxCoordinate), designation});
        }
    }
    if (object.contains("annotationSelection")) {
        const auto entries = array(object.value("annotationSelection"));
        if (entries.size() > 256) { invalid(); }
        for (const auto& value : entries) {
            const auto entry = value.toObject();
            const int id = entry.value("sourceId").toInt(-1), view = entry.value("view").toInt(-1);
            if (id < 1 || id > 10000 || view < 0 || view > 2) { invalid(); }
            const AnnotationTransfer request{id, view};
            if (std::ranges::find(result.annotationSelection, request) != result.annotationSelection.end()) { invalid(); }
            result.annotationSelection.push_back(request);
        }
        const auto snapshots = array(object.value("annotations"));
        if (snapshots.size() > entries.size()) { invalid(); }
        for (const auto& value : snapshots) {
            const auto entry = value.toObject();
            const int id = entry.value("sourceId").toInt(-1), view = entry.value("view").toInt(-1);
            const int kind = entry.value("kind").toInt(-1);
            const auto a = readPoint(entry.value("a")), b = readPoint(entry.value("b"));
            const auto text = entry.value("text").toString();
            const auto size = entry.value("value").toDouble(-1);
            if (kind < 0 || kind > 3 || size < 0 || size > maxCoordinate || text.size() > 160
                || !entry.value("faceOn").isBool()
                || std::ranges::find(result.annotationSelection, AnnotationTransfer{id, view}) == result.annotationSelection.end()) { invalid(); }
            result.annotations.push_back({id, view, static_cast<ModelAnnotationKind>(kind),
                {a.x, a.y}, {b.x, b.y}, size, text, entry.value("faceOn").toBool()});
        }
    }
    return result;
}

QJsonObject writeModelViews(const ModelViews& model, const QString& draftPath, bool references)
{
    if (!model.geometry || model.sourcePath.isEmpty() || model.bodyId < 1 || model.bodyId > 10000
        || model.sourceDigest.size() != 32) {
        invalid();
    }
    validateSnapshot(*model.geometry, model.additional);
    const auto& views = model.geometry->views;
    const auto path = QFileInfo(draftPath).absoluteDir().relativeFilePath(model.sourcePath);
    QJsonArray layout;
    for (const auto& slot : model.layout) {
        if (!std::isfinite(slot.scale) || slot.scale <= 0 || slot.scale > maxScale
            || !std::isfinite(slot.center.x()) || !std::isfinite(slot.center.y())
            || std::abs(slot.center.x()) > maxCoordinate || std::abs(slot.center.y()) > maxCoordinate) {
            invalid();
        }
        layout.append(QJsonObject{{"center", writePoint({slot.center.x(), slot.center.y()})}, {"scale", slot.scale}});
    }
    QJsonObject result{{"source", path}, {"body", model.bodyId}, {"digest", QString::fromLatin1(model.sourceDigest.toHex())},
                       {"dimensions", model.dimensions}, {"hiddenLines", model.hiddenLines}, {"layout", layout},
                       {"front", writeView(views.front, references)}, {"top", writeView(views.top, references)},
                       {"right", writeView(views.right, references)}};
    if (model.variant) { result.insert("variant", *model.variant); }
    if (!model.specificationPath.isEmpty()) {
        result.insert("specification", QFileInfo(draftPath).absoluteDir().relativeFilePath(model.specificationPath));
    }
    if (!model.threads.empty()) {
        QJsonArray entries;
        for (const auto& thread : model.threads) {
            entries.append(QJsonObject{{"view", thread.view}, {"center", writePoint({thread.center.x(), thread.center.y()})},
                {"diameter", thread.diameter}, {"designation", thread.designation}});
        }
        result.insert("threads", entries);
    }
    if (!model.annotationSelection.empty()) {
        if (model.annotationSelection.size() > 256 || model.annotations.size() > model.annotationSelection.size()) { invalid(); }
        QJsonArray selection, snapshots;
        for (const auto& request : model.annotationSelection) {
            selection.append(QJsonObject{{"sourceId", request.sourceId}, {"view", request.view}});
        }
        for (const auto& annotation : model.annotations) {
            snapshots.append(QJsonObject{{"sourceId", annotation.sourceId}, {"view", annotation.view},
                {"kind", static_cast<int>(annotation.kind)}, {"a", writePoint({annotation.first.x(), annotation.first.y()})},
                {"b", writePoint({annotation.second.x(), annotation.second.y()})}, {"value", annotation.value},
                {"text", annotation.text}, {"faceOn", annotation.faceOn}});
        }
        result.insert("annotationSelection", selection);
        result.insert("annotations", snapshots);
    }
    if (model.geometry->section) {
        result.insert("section", writeSection(*model.geometry->section, references));
    }
    const auto validPoint = [](QPointF point) {
        return std::isfinite(point.x()) && std::isfinite(point.y())
            && std::abs(point.x()) <= maxCoordinate && std::abs(point.y()) <= maxCoordinate;
    };
    const auto validPositive = [](double value, double limit) {
        return std::isfinite(value) && value > 0 && value <= limit;
    };
    if (model.detail) {
        const auto& d = *model.detail;
        if (d.source < 0 || d.source > 2 || !validPoint(d.center) || !validPoint(d.placement.center)
            || !validPositive(d.radius, maxCoordinate) || !validPositive(d.placement.scale, maxScale)) {
            invalid();
        }
        result.insert("detail", QJsonObject{{"view", d.source}, {"focus", writePoint({d.center.x(), d.center.y()})},
            {"radius", d.radius}, {"center", writePoint({d.placement.center.x(), d.placement.center.y()})},
            {"scale", d.placement.scale}});
    }
    if (model.local) {
        const auto& l = *model.local;
        if (!model.geometry->section || !validPoint(l.center) || !validPositive(l.radius, maxCoordinate)) {
            invalid();
        }
        result.insert("local", QJsonObject{{"focus", writePoint({l.center.x(), l.center.y()})}, {"radius", l.radius}});
    }
    if (references && (!model.titleData.name.isEmpty() || !model.titleData.designation.isEmpty() || model.titleData.massKg)) {
        QJsonObject title{{"name", model.titleData.name}, {"designation", model.titleData.designation}};
        if (model.titleData.massKg) {
            title.insert("mass", *model.titleData.massKg);
        }
        result.insert("titleData", title);
    }
    if (!model.additional.empty()) {
        result.insert("additional", writeAdditionalViews(model));
    }
    if (std::ranges::find(model.showBase, false) != model.showBase.end()) {
        QJsonArray visibility;
        for (const bool visible : model.showBase) {
            visibility.append(visible);
        }
        result.insert("baseVisible", visibility);
    }
    if (model.assembly) {
        result.insert("assembly", writeAssemblyDrawing(*model.assembly));
    }
    return result;
}

}  // namespace graph3d::io
