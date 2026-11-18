#include "geometry/Primitives.h"
#include "geometry/SketchConstraints.h"
#include "geometry/SketchCurves.h"
#include "io/Exchange.h"
#include "io/ProjectIO.h"

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <Bnd_Box.hxx>
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

#include <algorithm>
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

std::array<double, 3> extents(const TopoDS_Shape& shape)
{
    Bnd_Box bounds;
    BRepBndLib::AddOptimal(shape, bounds, false, false);
    const auto low = bounds.CornerMin();
    const auto high = bounds.CornerMax();
    return {high.X() - low.X(), high.Y() - low.Y(), high.Z() - low.Z()};
}

bool inside(const TopoDS_Shape& shape, double x, double y, double z)
{
    return BRepClass3d_SolidClassifier(shape, gp_Pnt(x, y, z), 1.e-6).State() == TopAbs_IN;
}

void checkShellAndStep(const QDir& folder)
{
    doc::Document document;
    const auto source = document.addPrimitive(geom::Primitive::Cube);
    const auto planes = geom::planarFaces(document.find(source)->shape);
    const auto top = std::ranges::find_if(planes, [](const auto& face) { return face.outward[2] > .99; });
    check(top != planes.end(), "Shell opening face was not found");
    const auto shell = document.addAdvanced(doc::Shell{source, {*top}, 4});
    check(std::abs(volume(document.find(shell)->shape) - 187456) < .01, "Initial shell volume is wrong");
    check(document.editAdvanced(shell, doc::Shell{source, {*top}, 6}), "Shell edit did not rebuild");
    check(document.setDensity(shell, 7850), "Density was not saved");
    const auto project = folder.filePath("wall-thickness.graph3d");
    io::saveProject(project, document);
    const auto saved = io::openProject(project);
    check(saved.find(shell)->densityKgM3 == 7850 && std::get<doc::Shell>(saved.find(shell)->parameters).thickness == 6,
          "Material density or shell inputs did not round trip");
    const auto step = folder.filePath("wall-thickness.step");
    io::exportVisible(step, saved, io::ExchangeFormat::Step);
    const auto imported = io::readStep(step);
    for (const auto& shape : {saved.find(shell)->shape, imported}) {
        const auto stats = geom::massProperties(shape, 7850);
        check(std::abs(stats.volumeMm3 - 272064) < .01 && std::abs(stats.massKg - 2.1357024) < 1.e-7
            && std::abs(stats.centerMm[0] - 50) < 1.e-6 && std::abs(stats.centerMm[1] - 50) < 1.e-6
            && stats.centerMm[2] < 50, "Shell measurements changed");
        check(inside(shape, 5.9, 50, 50) && !inside(shape, 6.1, 50, 50)
            && inside(shape, 50, 50, 5.9) && !inside(shape, 50, 50, 6.1), "Wrong shell wall or bottom thickness");
    }
    check(document.undo() && document.undo() && std::abs(volume(document.find(shell)->shape) - 187456) < .01,
          "Shell edit was not undoable");
    check(document.editAdvanced(shell, doc::Shell{source, {*top}, 60})
        && document.find(shell)->status == doc::Status::Failed, "Infeasible wall thickness was accepted");
}

void checkSweepAndStep(const QDir& folder)
{
    doc::Document document;
    const auto datum = document.addAdvanced(geom::DatumPlane{});
    geom::Sketch circle{geom::Plane::XY, {geom::Circle{{10, 5}, 5}}};
    circle.datumPlane = datum;
    const auto profile = document.addSketch(circle);
    doc::Sweep path{profile, {{10, 5, 0}, {10, 5, 40}, {70, 5, 40}}};
    const auto result = document.addAdvanced(path);
    const auto initial = geom::massProperties(document.find(result)->shape, 0);
    check(std::abs(initial.volumeMm3 - 2500 * std::numbers::pi) < .01, "Sweep did not follow the bend");
    path.path.back()[0] = 90;
    check(document.editAdvanced(result, path), "Sweep path did not rebuild");
    const auto project = folder.filePath("swept-profile.graph3d");
    io::saveProject(project, document);
    const auto saved = io::openProject(project);
    check(std::get<doc::Sweep>(saved.find(result)->parameters) == path && saved.find(datum)->visible,
          "Sweep or construction plane was lost on reopening");
    const auto step = folder.filePath("swept-profile.step");
    io::exportVisible(step, saved, io::ExchangeFormat::Step);
    const auto imported = io::readStep(step);
    const auto dimensions = extents(saved.find(result)->shape);
    const auto dimensionsAfter = extents(imported);
    check(std::abs(dimensions[0] - 85) < 1.e-5 && std::abs(dimensions[1] - 10) < 1.e-5
        && std::abs(dimensions[2] - 45) < 1.e-5, "Sweep control dimensions are wrong");
    for (int i = 0; i < 3; ++i) {
        check(std::abs(dimensions[i] - dimensionsAfter[i]) < 1.e-5, "STEP changed a sweep control dimension");
    }
    check(std::abs(volume(imported) - 3000 * std::numbers::pi) < .01, "STEP changed swept volume");
}

void checkOtherAdvancedFeatures(const QDir& folder)
{
    doc::Document document;
    const auto datum = document.addAdvanced(geom::DatumPlane{{0, 0, 30}, {0, 0, 1}});
    const auto lower = document.addSketch({geom::Plane::XY, {geom::Circle{{0, 0}, 10}}});
    geom::Sketch upperSketch{geom::Plane::XY, {geom::Circle{{0, 0}, 20}}};
    upperSketch.datumPlane = datum;
    const auto upper = document.addSketch(upperSketch);
    const auto loft = document.addAdvanced(doc::Loft{{lower, upper}, true});
    check(std::abs(volume(document.find(loft)->shape) - 7000 * std::numbers::pi) < .01, "Loft has the wrong volume");
    check(document.editAdvanced(datum, geom::DatumPlane{{0, 0, 40}, {0, 0, 1}})
        && std::abs(extents(document.find(loft)->shape)[2] - 40) < 1.e-6,
        "Loft did not rebuild after editing its construction plane");
    const auto axis = document.addAdvanced(geom::DatumAxis{{0, 0, 0}, {0, 1, 0}});
    const auto profile = document.addSketch({geom::Plane::XY, {geom::Rectangle{{10, 0}, {20, 30}}}});
    const auto revolved = document.addRevolve(profile, geom::Axis::U, 0, 360, axis);
    check(std::abs(volume(document.find(revolved)->shape) - 9000 * std::numbers::pi) < .01,
          "Revolve ignored its construction axis");
    check(document.editAdvanced(axis, geom::DatumAxis{{5, 0, 0}, {0, 1, 0}})
        && std::abs(volume(document.find(revolved)->shape) - 6000 * std::numbers::pi) < .01,
        "Revolve did not rebuild after editing its construction axis");
    const auto a = document.addImported("Box A", geom::makePrimitive(geom::Primitive::Cube, 0, 200));
    const auto b = document.addImported("Box B", geom::makePrimitive(geom::Primitive::Cube, 140, 200));
    check(std::abs(geom::minimumDistance(document.find(a)->shape, document.find(b)->shape) - 40) < 1.e-6,
          "Minimum distance has the wrong value");
    const auto planes = geom::planarFaces(document.find(a)->shape);
    const auto select = [&](int i, double direction) {
        const auto found = std::ranges::find_if(planes, [&](const auto& face) { return face.outward[i] * direction > .99; });
        check(found != planes.end(), "A draft face was not found");
        return *found;
    };
    const auto neutral = select(2, -1);
    const auto wall = select(0, 1);
    const auto draft = document.addAdvanced(doc::DraftAngle{a, neutral, {wall}, 5});
    const auto draftVolume = volume(document.find(draft)->shape);
    check(std::abs(std::abs(draftVolume - 1.e6) - 5.e5 * std::tan(5 * std::numbers::pi / 180)) < .01,
          "Draft did not move the wall by the specified angle");
    check(document.editAdvanced(draft, doc::DraftAngle{a, neutral, {wall}, 8})
        && document.find(draft)->status == doc::Status::Ready
        && std::abs(volume(document.find(draft)->shape) - draftVolume) > 1,
        "Draft angle edit did not change the result");

    std::vector<geom::EdgeReference> edges;
    for (const auto& edge : geom::twoFaceEdges(document.find(b)->shape)) {
        if (std::abs(edge.samples.front()[2] - edge.samples.back()[2]) > 99 && edges.size() < 2) {
            edges.push_back(edge);
        }
    }
    check(edges.size() == 2, "Test edges were not found");
    const auto chamfer = document.addEdgeTreatment(b, edges, geom::EdgeTreatmentKind::Chamfer, 3);
    const auto rounded = document.addEdgeTreatment(b, edges, geom::EdgeTreatmentKind::Fillet, 3);
    check(std::abs(volume(document.find(chamfer)->shape) - 999100) < .01
        && std::abs(volume(document.find(rounded)->shape) - (1.e6 - 1800 * (1 - std::numbers::pi / 4))) < .01,
        "Multi-edge profiling did not process every edge");
    const auto path = folder.filePath("advanced-operations.graph3d");
    io::saveProject(path, document);
    const auto restored = io::openProject(path);
    check(std::get<doc::EdgeTreatment>(restored.find(chamfer)->parameters).additionalEdges.size() == 1
        && std::get<doc::Revolve>(restored.find(revolved)->parameters).datumAxis == axis
        && std::get<doc::Loft>(restored.find(loft)->parameters).profiles == std::vector<int>{lower, upper}
        && std::get<doc::DraftAngle>(restored.find(draft)->parameters).degrees == 8
        && restored.find(rounded)->status == doc::Status::Ready, "Advanced operations did not round trip");
}

void curvedSketches(const QDir& folder)
{
    using enum geom::ConstraintKind;
    doc::Document model;
    geom::Sketch wedge{geom::Plane::XY,
        {geom::Line{{40, 0}, {15, 0}}, geom::Arc{{15, 0}, {4.3933982822, 4.3933982822}, {0, 15}},
         geom::Line{{0, 15}, {0, 40}}, geom::Line{{0, 40}, {40, 0}}},
        {{Tangent, {0, 0}, {1, 0}}, {Tangent, {2, 0}, {1, 0}}, {Angle, {0, 0}, {2, 0}, 90}}};
    const auto sketch = model.addSketch(wedge);
    const auto block = model.addExtrude(sketch, 10);
    const double before = volume(model.find(block)->shape);
    wedge.constraints[2].value = 110;
    const auto solved = geom::solveSketch(wedge);
    check(solved.conflicts.empty(), "Changing the angle did not preserve both tangent connections");
    const auto arcEntity = std::get<geom::Arc>(solved.entities[1]);
    const auto arc = geom::sketchArc(arcEntity);
    check(arc.has_value(), "The solved arc disappeared");
    const auto tangentError = [&](int index, geom::Point2 contact) {
        const auto line = std::get<geom::Line>(solved.entities[index]);
        return (contact.u - arc->center.u) * (line.end.u - line.start.u)
             + (contact.v - arc->center.v) * (line.end.v - line.start.v);
    };
    check(std::abs(tangentError(0, arcEntity.start)) < 1.e-5 && std::abs(tangentError(2, arcEntity.end)) < 1.e-5,
        "The resulting arc is not tangent to both lines");
    check(model.editSketch(sketch, wedge) && model.find(block)->status == doc::Status::Ready,
        "Changing the angle did not rebuild the existing extrusion");
    const double after = volume(model.find(block)->shape);
    check(after > 0 && std::abs(after - before) > 1, "The driving angle did not change the extrusion");
    const auto revision = model.revision();
    auto conflict = wedge;
    conflict.constraints.push_back({Angle, {0, 0}, {2, 0}, 80});
    const auto failed = geom::solveSketch(conflict);
    check(std::ranges::find(failed.conflicts, 3) != failed.conflicts.end(), "The contradictory angle was not reported");
    rejects([&] { model.editSketch(sketch, conflict); }, "A conflicting sketch update was accepted");
    check(model.revision() == revision && std::abs(volume(model.find(block)->shape) - after) < 1.e-7,
        "A conflicting sketch update changed the valid extrusion");

    const auto profile = model.addSketch({geom::Plane::XY, {geom::Circle{{0, 0}, 1}}});
    geom::Sketch route{geom::Plane::XZ, {geom::Arc{{0, 0}, {5.85786437627, 14.1421356237}, {20, 20}},
                                      geom::Line{{-7, 2}, {-8, 5}}}, {}, {}, 0, {1}, true};
    const auto pathSketch = model.addSketch(route);
    const auto swept = model.addAdvanced(doc::Sweep{profile, {}, pathSketch});
    const double sweptVolume = volume(model.find(swept)->shape);
    check(model.find(swept)->status == doc::Status::Ready
        && std::abs(sweptVolume - 10 * std::numbers::pi * std::numbers::pi) < 0.1,
        "An exact circular arc did not produce the expected swept solid");
    auto reversed = route;
    auto& reverseArc = std::get<geom::Arc>(reversed.entities[0]);
    std::swap(reverseArc.start, reverseArc.end);
    const auto reverseSketch = model.addSketch(reversed);
    const auto reverseSweep = model.addAdvanced(doc::Sweep{profile, {}, reverseSketch, true});
    check(model.find(reverseSweep)->status == doc::Status::Ready
        && std::abs(volume(model.find(reverseSweep)->shape) - sweptVolume) < 0.01,
        "Reversing a path did not restore the intended start and sweep");

    route.entities[0] = geom::Spline{{{0, 0}, {0, 12}, {10, 25}, {24, 32}}};
    check(model.editSketch(pathSketch, route) && model.find(swept)->status == doc::Status::Ready
        && std::abs(volume(model.find(swept)->shape) - sweptVolume) > 5,
        "Changing the sketch to a spline did not rebuild the dependent sweep");

    const auto path = folder.filePath("curved-sketches.graph3d");
    io::saveProject(path, model);
    check(QJsonDocument::fromJson(read(path)).object().value("version").toInt() == 8, "Expected project version 8");
    const auto restored = io::openProject(path);
    const auto& savedPath = std::get<geom::Sketch>(restored.find(pathSketch)->parameters);
    const auto& savedWedge = std::get<geom::Sketch>(restored.find(sketch)->parameters);
    check(savedPath.trajectory && savedPath.construction == std::vector<int>{1}
        && std::get<geom::Spline>(savedPath.entities[0]).points.size() == 4
        && std::holds_alternative<geom::Arc>(savedWedge.entities[1]) && savedWedge.constraints[2].value == 110
        && std::get<doc::Sweep>(restored.find(swept)->parameters).pathSketch == pathSketch
        && std::get<doc::Sweep>(restored.find(reverseSweep)->parameters).reversePath
        && restored.find(reverseSweep)->status == doc::Status::Ready
        && restored.find(swept)->status == doc::Status::Ready && restored.find(block)->status == doc::Status::Ready,
        "Curves, construction mode, driving constraints or the sweep link failed to round trip");
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
        checkShellAndStep(folder);
        checkSweepAndStep(folder);
        checkOtherAdvancedFeatures(folder);
        curvedSketches(folder);
    } catch (const Standard_Failure& error) {
        std::cerr << "Open CASCADE: " << error.GetMessageString() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
