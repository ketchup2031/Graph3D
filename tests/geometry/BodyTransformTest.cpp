#include "document/Document.h"
#include "geometry/Primitives.h"
#include "io/Exchange.h"
#include "io/ProjectIO.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Pnt.hxx>

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace doc = graph3d::document;
namespace geom = graph3d::geometry;
namespace io = graph3d::io;
using enum geom::ModelAxis;

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<gp_Pnt> solidCenters(const TopoDS_Shape& shape, double volume)
{
    std::vector<gp_Pnt> centers;
    check(!shape.IsNull(), "Missing result shape");
    for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More(); explorer.Next()) {
        check(BRepCheck_Analyzer(explorer.Current()).IsValid(), "Invalid result solid");
        GProp_GProps props;
        BRepGProp::VolumeProperties(explorer.Current(), props);
        check(std::abs(props.Mass() - volume) < 1.e-4, "Wrong solid volume or orientation");
        centers.push_back(props.CentreOfMass());
    }
    return centers;
}

void checkCenters(const TopoDS_Shape& shape, double volume, std::initializer_list<gp_Pnt> expected)
{
    const auto centers = solidCenters(shape, volume);
    check(centers.size() == expected.size(), "Wrong number of instances");
    for (const auto& point : expected) {
        bool found = false;
        for (const auto& center : centers) {
            found |= center.Distance(point) < 1.e-6;
        }
        check(found, "Missing instance at expected position");
    }
}

void geometry()
{
    const auto cube = geom::makePrimitive(geom::Primitive::Cube, 100, 20);
    checkCenters(geom::transformBody(cube, geom::LinearArray{Y, 3, -120}), 1'000'000,
                 {{150, 70, 50}, {150, -50, 50}, {150, -170, 50}});
    checkCenters(geom::transformBody(cube, geom::PolarArray{Z, 4, 360}), 1'000'000,
                 {{150, 70, 50}, {-70, 150, 50}, {-150, -70, 50}, {70, -150, 50}});
    checkCenters(geom::transformBody(cube, geom::PolarArray{X, 3, 180}), 1'000'000,
                 {{150, 70, 50}, {150, -50, 70}, {150, -70, -50}});
    checkCenters(geom::transformBody(cube, geom::Mirror{geom::Plane::YZ, 20}), 1'000'000,
                 {{-110, 70, 50}});
    checkCenters(geom::transformBody(cube, geom::Mirror{geom::Plane::XZ, -10}), 1'000'000,
                 {{150, -90, 50}});
    checkCenters(geom::transformBody(cube, geom::Mirror{geom::Plane::XY, 0}), 1'000'000,
                 {{150, 70, -50}});
}

void modelAndProject()
{
    doc::Document document;
    const auto sketch = document.addSketch({geom::Plane::XY, {geom::Rectangle{{100, 20}, {200, 120}}}});
    const auto extrude = document.addExtrude(sketch, 40);
    const geom::LinearArray linear{X, 2, 120};
    const auto array = document.addTransform(extrude, linear);
    const auto circular = document.addTransform(array, geom::PolarArray{Z, 2, 90});
    const auto mirror = document.addTransform(circular, geom::Mirror{geom::Plane::XY, -10});
    check(!document.find(extrude)->visible && document.find(mirror)->visible, "Unexpected result visibility");
    checkCenters(document.find(mirror)->shape, 400'000,
                 {{150, 70, -40}, {270, 70, -40}, {-70, 150, -40}, {-70, 270, -40}});
    check(!document.editTransform(array, linear), "No-op created history");
    check(document.editExtrude(extrude, 50), "Could not edit source");
    checkCenters(document.find(mirror)->shape, 500'000,
                 {{150, 70, -45}, {270, 70, -45}, {-70, 150, -45}, {-70, 270, -45}});
    check(document.editTransform(array, geom::LinearArray{X, 2, 0}), "Could not edit spacing");
    check(document.find(array)->status == doc::Status::Failed && document.find(mirror)->status == doc::Status::Blocked
          && document.find(mirror)->shape.IsNull(), "Invalid array left a stale dependent");
    check(document.undo() && document.find(mirror)->status == doc::Status::Ready, "Undo did not restore result");
    check(document.redo() && document.find(mirror)->status == doc::Status::Blocked, "Redo did not restore error");
    check(document.undo(), "Could not restore valid array");
    const auto hole = document.addHole(extrude, {{150, 70, 50}, {0, 0, 1}}, {10, 20, false});
    check(document.setVisible(hole, false), "Could not hide test hole");
    QTemporaryDir folder;
    check(folder.isValid(), "Could not create test folder");
    const auto path = folder.filePath("arrays.graph3d");
    io::saveProject(path, document);
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Could not read saved project");
    check(QJsonDocument::fromJson(file.readAll()).object().value("version").toInt() == 3,
          "Project with a transform and later hole must use version 3");
    file.close();
    auto reopened = io::openProject(path);
    for (const auto id : {array, circular, mirror}) {
        const auto& original = std::get<doc::Transform>(document.find(id)->parameters);
        const auto& restored = std::get<doc::Transform>(reopened.find(id)->parameters);
        check(original.body == restored.body && original.parameters == restored.parameters,
              "Transform parameters did not survive the round trip");
    }
    check(reopened.editTransform(array, geom::LinearArray{X, 2, 160}), "Could not edit saved parameters");
    checkCenters(reopened.find(mirror)->shape, 500'000,
                 {{150, 70, -45}, {310, 70, -45}, {-70, 150, -45}, {-70, 310, -45}});
    const auto step = folder.filePath("arrays.step");
    io::exportVisible(step, reopened, io::ExchangeFormat::Step);
    checkCenters(io::readStep(step), 500'000,
                 {{150, 70, -45}, {310, 70, -45}, {-70, 150, -45}, {-70, 310, -45}});
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        geometry();
        modelAndProject();
    } catch (const Standard_Failure& error) {
        std::cerr << error.GetMessageString() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
