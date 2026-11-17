#include "io/ModelViewsIO.h"
#include "io/AssemblyDrawingIO.h"

#include "io/FileIO.h"
#include "io/ProjectIO.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

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
    return view;
}

QJsonObject writeView(const View& view)
{
    QJsonArray circles;
    for (const auto& circle : view.circles) {
        circles.append(QJsonObject{{"center", writePoint(circle.center)}, {"radius", circle.radius}});
    }
    return {{"width", view.width}, {"height", view.height},
            {"visible", writeEdges(view.visible)}, {"hidden", writeEdges(view.hidden)},
            {"circles", circles}};
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

QJsonObject writeSection(const Section& cut)
{
    QJsonArray material;
    for (const auto& face : cut.material) {
        material.append(writeEdges(face));
    }
    return {{"plane", planes.at(static_cast<std::size_t>(cut.plane))}, {"offset", cut.offset},
            {"view", writeView(cut.view)}, {"material", material}};
}

void validateSnapshot(const ModelProjection& snapshot)
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
        consume(remaining, value.circles.size());
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
                           QSizeF paper, const ModelViews* previous)
{
    const auto* item = source.document.find(bodyId);
    if (!item || !canProjectBody(*item)) {
        fileError(QCoreApplication::translate("ModelViews", "Body #%1 is unavailable in the source model.").arg(bodyId));
    }
    auto snapshot = std::make_shared<ModelProjection>();
    snapshot->views = project(item->shape);
    if (cut) {
        snapshot->section = section(item->shape, cut->plane, cut->offset);
    }
    validateSnapshot(*snapshot);
    auto model = previous ? *previous : ModelViews{};
    model.sourcePath = source.path;
    model.bodyId = bodyId;
    model.sourceDigest = source.digest;
    if (!previous) {
        model.layout = defaultViewLayout(snapshot->views, paper);
    }
    model.geometry = std::move(snapshot);
    const auto prior = previous && previous->bodyId == bodyId && previous->sourcePath == source.path
        ? previous->assembly.get() : nullptr;
    model.assembly = buildAssemblyDrawing(source, *item, prior);
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
    if (object.contains("assembly")) {
        result.assembly = std::make_shared<AssemblyDrawing>(readAssemblyDrawing(object.value("assembly")));
    }
    return result;
}

QJsonObject writeModelViews(const ModelViews& model, const QString& draftPath)
{
    if (!model.geometry || model.sourcePath.isEmpty() || model.bodyId < 1 || model.bodyId > 10000
        || model.sourceDigest.size() != 32) {
        invalid();
    }
    validateSnapshot(*model.geometry);
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
                       {"front", writeView(views.front)}, {"top", writeView(views.top)},
                       {"right", writeView(views.right)}};
    if (model.geometry->section) {
        result.insert("section", writeSection(*model.geometry->section));
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
    if (model.assembly) {
        result.insert("assembly", writeAssemblyDrawing(*model.assembly));
    }
    return result;
}

}  // namespace graph3d::io
