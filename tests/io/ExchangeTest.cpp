#include "geometry/Primitives.h"
#include "io/Exchange.h"
#include "io/ProjectIO.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>

namespace doc = graph3d::document;
namespace geom = graph3d::geometry;
namespace io = graph3d::io;

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class F>
void rejects(F action, const char* message)
{
    try {
        action();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

double volume(const TopoDS_Shape& shape)
{
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return properties.Mass();
}

void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Could not write test fixture");
}

QByteArray read(const QString& path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "File was not written");
    return file.readAll();
}

double stlVolume(const QString& path)
{
    const auto bytes = read(path);
    QDataStream stream(bytes);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    check(stream.skipRawData(80) == 80, "Missing STL header");
    quint32 count;
    stream >> count;
    check(count > 0 && bytes.size() == 84 + static_cast<qint64>(count) * 50, "Invalid binary STL length");
    double signedVolume = 0;
    for (quint32 i = 0; i < count; ++i) {
        float nx, ny, nz;
        stream >> nx >> ny >> nz;
        std::array<gp_XYZ, 3> vertices;
        for (auto& vertex : vertices) {
            float x, y, z;
            stream >> x >> y >> z;
            vertex.SetCoord(x, y, z);
        }
        quint16 attribute;
        stream >> attribute;
        const gp_XYZ cross = (vertices[1] - vertices[0]).Crossed(vertices[2] - vertices[0]);
        check(cross.Dot(gp_XYZ(nx, ny, nz)) > 0 && attribute == 0, "Invalid STL facet winding or normal");
        signedVolume += vertices[0].Dot(vertices[1].Crossed(vertices[2])) / 6;
    }
    check(stream.status() == QDataStream::Ok, "Could not read every STL triangle");
    return signedVolume;
}

void checkExports(const QDir& folder, const QString& name, const doc::Document& document, double expected)
{
    const auto step = folder.filePath(name + ".step");
    const auto stl = folder.filePath(name + ".stl");
    io::exportVisible(step, document, io::ExchangeFormat::Step);
    io::exportVisible(stl, document, io::ExchangeFormat::Stl);
    check(std::abs(volume(io::readStep(step)) - expected) < expected * 1e-7, "STEP round trip changed volume or units");
    check(std::abs(stlVolume(stl) - expected) < expected * 0.01, "STL volume or orientation is incorrect");
}

void exports(const QDir& folder)
{
    doc::Document cube;
    cube.addPrimitive(geom::Primitive::Cube);
    const auto hidden = cube.addPrimitive(geom::Primitive::Sphere);
    cube.setVisible(hidden, false);
    checkExports(folder, "cube", cube, 1'000'000);

    doc::Document cylinder;
    const auto circle = cylinder.addSketch({geom::Plane::XY, {geom::Circle{{20, 0}, 20}}});
    cylinder.addExtrude(circle, 30);
    cylinder.setVisible(circle, true);
    checkExports(folder, "extrusion", cylinder, 12'000 * std::numbers::pi);

    doc::Document ring;
    const auto profile = ring.addSketch({geom::Plane::XZ, {geom::Rectangle{{10, 0}, {20, 30}}}});
    ring.addRevolve(profile, geom::Axis::V, 0, 360);
    checkExports(folder, "revolution", ring, 9000 * std::numbers::pi);

    cube.setVisible(hidden, true);
    checkExports(folder, "assembly", cube, 1'000'000 + 500'000 * std::numbers::pi / 3);

    const auto blank = folder.filePath("blank.stl");
    write(blank, "existing file");
    rejects([&] { io::exportVisible(blank, doc::Document{}, io::ExchangeFormat::Stl); }, "Empty document was exported");
    check(read(blank) == "existing file", "Failed export overwrote existing file");
    const auto junk = folder.filePath("bad.step");
    write(junk, "not STEP");
    rejects([&] { io::readStep(junk); }, "Invalid STEP was accepted");
}

void roundTrip(const QDir& folder)
{
    doc::Document original;
    original.addPrimitive(geom::Primitive::Sphere);
    const auto outline = original.addSketch({geom::Plane::XZ, {geom::Rectangle{{10, 0}, {20, 30}}}});
    const auto body = original.addRevolve(outline, geom::Axis::V, 0, 360);
    original.editRevolve(body, geom::Axis::V, 5, 90);
    const auto poly = original.addSketch({geom::Plane::XY, {
        geom::Line{{0,0}, {20,0}}, geom::Line{{20,0}, {0,20}}, geom::Line{{0,20}, {0,0}}
    }});
    const auto prism = original.addExtrude(poly, 15);
    const auto result = original.addBoolean(body, prism, geom::BooleanOperation::Union);
    original.editExtrude(prism, 0);

    // Import a model with a Unicode name; the saved project must retain the body after moving its source.
    const auto unicodeStep = folder.filePath(QStringLiteral("деталь.step"));
    write(unicodeStep, read(folder.filePath("cube.step")));
    const auto imported = original.addImported(QStringLiteral("деталь.step").toStdString(), io::readStep(unicodeStep));
    const auto path = folder.filePath(QStringLiteral("проект.graph3d"));
    io::saveProject(path, original);
    check(QFile::remove(unicodeStep), "Could not remove temporary import source");

    auto loaded = io::openProject(path);
    check(!loaded.canUndo() && !loaded.canRedo() && loaded.revision() == 0, "Open must establish a clean history");
    check(loaded.objects().size() == original.objects().size(), "Object count changed across save/open");
    for (const auto& object : original.objects()) {
        const auto& copy = *loaded.find(object.id);
        check(copy.parameters.index() == object.parameters.index() && copy.visible == object.visible
              && copy.status == object.status, "An object or its state changed across save/open");
    }
    const auto& r = std::get<doc::Revolve>(loaded.find(body)->parameters);
    check(r.profile == outline && r.axis == geom::Axis::V && r.offset == 5 && r.angle == 90,
          "Revolve parameters were not preserved");
    const auto& e = std::get<doc::Extrude>(loaded.find(prism)->parameters);
    check(e.profile == poly && e.distance == 0, "Failed extrusion parameters were lost");
    check(loaded.find(result)->status == doc::Status::Blocked, "Dependency failure was not reconstructed");
    const auto& importedShape = std::get<doc::Imported>(loaded.find(imported)->parameters);
    check(importedShape.name == QStringLiteral("деталь.step").toStdString()
          && std::abs(volume(loaded.find(imported)->shape) - 1'000'000) < 0.1,
          "Embedded imported solid or name did not survive");
    check(loaded.editExtrude(prism, 15) && loaded.find(result)->status == doc::Status::Ready,
          "Dependencies did not connect after opening the project");
    auto json = QJsonDocument::fromJson(read(path)).object();
    const auto damaged = folder.filePath("damaged.graph3d");
    json["version"] = 999;
    write(damaged, QJsonDocument(json).toJson());
    rejects([&] { io::openProject(damaged); }, "A future format version was accepted");
    json["version"] = 1;
    auto objects = json.value("objects").toArray();
    auto forward = objects.at(body - 1).toObject();
    forward["profile"] = imported;
    objects[body - 1] = forward;
    json["objects"] = objects;
    write(damaged, QJsonDocument(json).toJson());
    rejects([&] { io::openProject(damaged); }, "A forward dependency was accepted");
    write(damaged, "{");
    rejects([&] { io::openProject(damaged); }, "Malformed JSON was accepted");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary;
        check(temporary.isValid(), "Could not create a test directory");
        const auto arguments = QCoreApplication::arguments();
        const QDir folder(arguments.size() == 2 ? arguments.at(1) : temporary.path());
        check(folder.exists(), "Fixture directory does not exist");
        exports(folder);
        roundTrip(folder);
    } catch (const Standard_Failure& error) {
        std::cerr << "Open CASCADE: " << error.GetMessageString() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
