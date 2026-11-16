#include "document/Document.h"
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

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

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

struct ExpectedSolid {
    gp_Pnt center;
    double volume;
};

void verifySolids(const TopoDS_Shape& shape, std::initializer_list<ExpectedSolid> expected)
{
    check(!shape.IsNull(), "Missing assembly geometry");
    std::vector<ExpectedSolid> unmatched(expected);
    for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More(); explorer.Next()) {
        check(BRepCheck_Analyzer(explorer.Current()).IsValid(), "Invalid component solid");
        GProp_GProps props;
        BRepGProp::VolumeProperties(explorer.Current(), props);
        const auto found = std::ranges::find_if(unmatched, [&](const auto& solid) {
            return props.CentreOfMass().Distance(solid.center) < 1.e-6
                && std::abs(props.Mass() - solid.volume) < 1.e-6;
        });
        check(found != unmatched.end(), "Unexpected component position or volume");
        unmatched.erase(found);
    }
    check(unmatched.empty(), "Missing component solid");
}

doc::ObjectId block(doc::Document& document, double width, double depth, double height)
{
    const auto sketch = document.addSketch({geom::Plane::XY, {geom::Rectangle{{0, 0}, {width, depth}}}});
    return document.addExtrude(sketch, height);
}

void assemblyProject()
{
    doc::Document document;
    const auto main = block(document, 20, 10, 8);
    const auto third = block(document, 4, 6, 5);
    doc::Assembly parameters{{
        {main, {0, {100, 0, 0}, {0, 0, 90}}},
        {main, {1, {30, 0, 0}, {180, 0, 0}}},
        {third, {0, {200, 0, 0}, {}}},
    }};
    const auto assembly = document.addAssembly(parameters);
    check(!document.find(main)->visible && !document.find(third)->visible, "Sources were not hidden");
    verifySolids(document.find(assembly)->shape, {{{95, 10, 4}, 1600}, {{105, 40, -4}, 1600}, {{202, 3, 2.5}, 120}});

    const auto mirror = document.addTransform(assembly, geom::Mirror{geom::Plane::XY, 0});
    check(!document.editAssembly(assembly, parameters), "Identical placement created an edit");
    parameters.components.front().placement.offset[1] = 25;
    check(document.editAssembly(assembly, parameters), "Could not move the assembly anchor");
    check(document.editExtrude(third, 9), "Could not edit the source of the third component");
    const std::initializer_list<ExpectedSolid> mirrored{
        {{95, 35, -4}, 1600}, {{105, 65, 4}, 1600}, {{202, 3, -4.5}, 216}
    };
    verifySolids(document.find(mirror)->shape, mirrored);

    auto invalid = parameters;
    invalid.components.front().placement.anchor = 2;
    check(document.editAssembly(assembly, invalid), "Could not update the invalid anchor");
    check(document.find(assembly)->status == doc::Status::Failed && document.find(mirror)->status == doc::Status::Blocked,
          "Invalid anchor did not block assembly dependents");
    check(document.undo(), "Could not undo the invalid anchor");
    verifySolids(document.find(mirror)->shape, mirrored);
    check(document.redo() && document.find(mirror)->status == doc::Status::Blocked,
          "Redo did not restore blocked state");
    check(document.undo(), "Could not restore the valid assembly");

    QTemporaryDir directory;
    check(directory.isValid(), "Could not create test directory");
    const auto path = directory.filePath("assembly.graph3d");
    io::saveProject(path, document);
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Could not read saved project");
    check(QJsonDocument::fromJson(file.readAll()).object().value("version").toInt() == 4,
          "Assembly did not use version 4");
    file.close();
    auto reopened = io::openProject(path);
    check(std::get<doc::Assembly>(reopened.find(assembly)->parameters).components == parameters.components,
          "Component parameters changed during round trip");
    verifySolids(reopened.find(mirror)->shape, mirrored);
    const auto step = directory.filePath("assembly.step");
    io::exportVisible(step, reopened, io::ExchangeFormat::Step);
    verifySolids(io::readStep(step), mirrored);
}

void coincidentComponents()
{
    doc::Document document;
    const auto source = block(document, 20, 10, 8);
    const auto assembly = document.addAssembly({{{source, {}}, {source, {}}}});
    verifySolids(document.find(assembly)->shape, {{{10, 5, 4}, 1600}, {{10, 5, 4}, 1600}});
    QTemporaryDir directory;
    check(directory.isValid(), "Could not create test directory");
    const auto step = directory.filePath("coincident.step");
    io::exportVisible(step, document, io::ExchangeFormat::Step);
    verifySolids(io::readStep(step), {{{10, 5, 4}, 1600}, {{10, 5, 4}, 1600}});
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        assemblyProject();
        coincidentComponents();
    } catch (const Standard_Failure& error) {
        std::cerr << error.GetMessageString() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
