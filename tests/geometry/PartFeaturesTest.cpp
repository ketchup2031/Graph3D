#include "document/Document.h"
#include "geometry/PartFeatures.h"
#include "geometry/Primitives.h"
#include "io/ProjectIO.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
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
void rejects(F operation, const char* message)
{
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void volumeIs(const TopoDS_Shape& shape, double expected)
{
    check(!shape.IsNull(), "Missing solid");
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    if (std::abs(properties.Mass() - expected) > expected * 1.e-7) {
        throw std::runtime_error("Unexpected volume " + std::to_string(properties.Mass())
                                 + ", expected " + std::to_string(expected));
    }
}

geom::FaceReference topFace(const TopoDS_Shape& body)
{
    for (TopExp_Explorer it(body, TopAbs_FACE); it.More(); it.Next()) {
        const auto face = TopoDS::Face(it.Current());
        const BRepAdaptor_Surface surface(face);
        if (surface.GetType() == GeomAbs_Plane && std::abs(surface.Plane().Location().Z() - 100) < 1.e-7
            && std::abs(surface.Plane().Axis().Direction().Z()) > 0.9999) {
            return geom::locateHole(face, {50, 50, 150}, {0, 0, -1});
        }
    }
    throw std::runtime_error("Missing top face of the reference cube");
}

geom::EdgeReference uprightEdge(const TopoDS_Shape& body)
{
    for (TopExp_Explorer it(body, TopAbs_EDGE); it.More(); it.Next()) {
        const auto ref = geom::referenceEdge(TopoDS::Edge(it.Current()));
        if (std::abs(ref.samples.front()[2] - ref.samples.back()[2]) > 99.9) {
            return ref;
        }
    }
    throw std::runtime_error("Missing upright cube edge");
}

void geometry()
{
    constexpr double pi = std::numbers::pi;
    const auto cube = geom::makePrimitive(geom::Primitive::Cube);
    const auto face = topFace(cube);
    check(face.center == geom::Point3{50, 50, 100} && face.outward[2] > 0.9999,
          "Hole center or drilling direction was incorrect");
    const auto edge = uprightEdge(cube);
    volumeIs(geom::drillHole(cube, face, {10, 20, false}), 1'000'000 - 500 * pi);
    volumeIs(geom::drillHole(cube, face, {10, 20, true}), 1'000'000 - 2500 * pi);
    volumeIs(geom::treatEdge(cube, edge, geom::EdgeTreatmentKind::Chamfer, 10), 995'000);
    volumeIs(geom::treatEdge(cube, edge, geom::EdgeTreatmentKind::Fillet, 10),
             1'000'000 - 100 * (100 - 25 * pi));
    volumeIs(cube, 1'000'000);

    rejects([&] { geom::drillHole(cube, face, {110, 20, true}); }, "Oversized hole was accepted");
    rejects([&] { geom::drillHole(cube, face, {10, 100, false}); }, "Blind hole breaking through was accepted");
    rejects([&] { geom::treatEdge(cube, edge, geom::EdgeTreatmentKind::Chamfer, 0); },
            "Zero chamfer distance was accepted");
    auto shifted = face;
    shifted.center[0] = -1;
    rejects([&] { geom::drillHole(cube, shifted, {10, 20, true}); }, "An outside center was accepted");

    auto reversed = edge;
    std::reverse(reversed.samples.begin(), reversed.samples.end());
    volumeIs(geom::treatEdge(cube, reversed, geom::EdgeTreatmentKind::Chamfer, 10), 995'000);
}

void dependencyAndFile()
{
    constexpr double pi = std::numbers::pi;
    doc::Document model;
    const auto sketch = model.addSketch({geom::Plane::XY, {geom::Rectangle{{0, 0}, {100, 100}}}});
    const auto prism = model.addExtrude(sketch, 100);
    const auto reference = topFace(model.find(prism)->shape);
    const auto hole = model.addHole(prism, reference, {10, 20, false});
    const auto edge = uprightEdge(model.find(hole)->shape);
    const auto chamfer = model.addEdgeTreatment(hole, edge, geom::EdgeTreatmentKind::Chamfer, 10);
    const auto fillet = model.addEdgeTreatment(prism, edge, geom::EdgeTreatmentKind::Fillet, 10);
    check(!model.find(prism)->visible && !model.find(hole)->visible, "Input bodies should be hidden");
    volumeIs(model.find(chamfer)->shape, 995'000 - 500 * pi);

    check(model.editHole(hole, {12, 20, true}), "Hole dimension edit failed");
    volumeIs(model.find(chamfer)->shape, 995'000 - 3600 * pi);
    check(model.editEdgeTreatment(chamfer, 5), "Chamfer parameter edit failed");
    volumeIs(model.find(chamfer)->shape, 998'750 - 3600 * pi);

    check(model.editExtrude(prism, 120), "Upstream geometry edit failed");
    check(model.find(hole)->status == doc::Status::Failed && model.find(hole)->shape.IsNull(),
          "A face that moved was silently reused");
    check(model.find(chamfer)->status == doc::Status::Blocked, "Chamfer should be blocked by a bad hole");
    check(model.find(fillet)->status == doc::Status::Failed && model.find(fillet)->shape.IsNull(),
          "A changed edge was silently reused");
    check(model.undo(), "Cannot undo upstream geometry change");
    volumeIs(model.find(chamfer)->shape, 998'750 - 3600 * pi);
    check(model.redo(), "Cannot redo upstream geometry change");
    check(model.find(chamfer)->status == doc::Status::Blocked, "Redone dependency failure was lost");
    check(model.undo(), "Cannot restore the good project");

    QTemporaryDir folder;
    check(folder.isValid(), "No temporary directory");
    const auto file = folder.filePath(QStringLiteral("teст.graph3d"));
    io::saveProject(file, model);
    auto reopened = io::openProject(file);
    check(!reopened.canUndo(), "Reopened file has an undo history");
    volumeIs(reopened.find(chamfer)->shape, 998'750 - 3600 * pi);
    volumeIs(reopened.find(fillet)->shape, 1'000'000 - 100 * (100 - 25 * pi));
    const auto& savedHole = std::get<doc::Hole>(reopened.find(hole)->parameters);
    check(savedHole.size.diameter == 12 && savedHole.size.through && savedHole.face.center == reference.center,
          "Reopened hole parameters or anchor differ");
    check(std::get<doc::EdgeTreatment>(reopened.find(chamfer)->parameters).edge.samples == edge.samples,
          "Reopened edge key differs");
    QFile input(file);
    check(input.open(QIODevice::ReadOnly), "Could not open saved project JSON");
    check(QJsonDocument::fromJson(input.readAll()).object().value("version").toInt() == 2,
          "Project with feature nodes should use version 2");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        geometry();
        dependencyAndFile();
    } catch (const Standard_Failure& error) {
        std::cerr << "Open CASCADE: " << error.GetMessageString() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
