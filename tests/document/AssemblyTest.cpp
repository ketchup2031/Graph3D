#include "document/Document.h"
#include "io/Exchange.h"
#include "io/LinkedComponents.h"
#include "io/ProjectIO.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Pnt.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <numbers>
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

geom::MateSurface findSurface(const TopoDS_Shape& shape, geom::MateKind kind, double size)
{
    const auto faces = geom::assemblySurfaces(shape);
    const auto found = std::ranges::find_if(faces, [=](const auto& face) {
        if (face.kind != kind) {
            return false;
        }
        return kind == geom::MateKind::Cylinder ? std::abs(face.radius - size) < 1.e-6
                                               : std::abs(face.direction[2] - size) < 1.e-6;
    });
    if (found == faces.end()) {
        std::cerr << "Missing mating surface " << static_cast<int>(kind) << " (" << size << "). Candidates:\n";
        for (const auto& face : faces) {
            std::cerr << static_cast<int>(face.kind) << " radius=" << face.radius << " direction=("
                << face.direction[0] << ", " << face.direction[1] << ", " << face.direction[2] << ")\n";
        }
        throw std::runtime_error("Expected mating surface was not found");
    }
    return *found;
}

void linkedAndMatedAssembly(const QDir& folder)
{
    doc::Document flange, bushing;
    const auto flangeProfile = flange.addSketch({geom::Plane::XY, {geom::Circle{{0, 0}, 25}, geom::Circle{{0, 0}, 10}}});
    const auto flangeBody = flange.addExtrude(flangeProfile, 10);
    flange.setPartProperties(flangeBody, {"G3D-016-01", "Flange", "Steel", "Base"});
    const auto radial = bushing.addSketch({geom::Plane::XZ, {geom::Rectangle{{4, 0}, {10, 20}}}});
    const auto bushingBody = bushing.addRevolve(radial, geom::Axis::V, 0, 360);
    bushing.setPartProperties(bushingBody, {"G3D-016-02", "Bushing", "Bronze", "Replaceable"});
    check(folder.mkpath("parts"), "Could not create assembly parts folder");
    const auto flangePath = folder.filePath("parts/flange.graph3d");
    const auto bushingPath = folder.filePath("parts/bushing.graph3d");
    const auto assemblyPath = folder.filePath("flange-and-bushing.graph3d");
    io::saveProject(flangePath, flange);
    io::saveProject(bushingPath, bushing);

    doc::Document document;
    const auto fixed = document.addLinkedComponent(io::readLinkedComponent(flangePath, flangeBody, assemblyPath));
    const auto moving = document.addLinkedComponent(io::readLinkedComponent(bushingPath, bushingBody, assemblyPath));
    doc::Assembly assembly{{{fixed, {}, true}, {moving, {0, {32, 14, 38}, {45, 12, 0}}}}, {}};
    const auto surface = [&](int id, geom::MateKind kind, double value) {
        return findSurface(document.find(id)->shape, kind, value);
    };
    using geom::MateKind;
    assembly.mates.push_back({2, 1, surface(moving, MateKind::Cylinder, 10), surface(fixed, MateKind::Cylinder, 10), false});
    assembly.mates.push_back({2, 1, surface(moving, MateKind::Plane, -1), surface(fixed, MateKind::Plane, 1), true});
    const auto free = document.assemblySolution(assembly);
    check(free.error.empty() && free.components[0].rotations == 0 && free.components[0].translations == 0
        && free.components[1].rotations == 1 && free.components[1].translations == 0,
        "Coaxial and end-face mates did not leave only axial rotation");
    const auto oneMate = [&] {
        auto coaxial = assembly;
        coaxial.mates.pop_back();
        return document.assemblySolution(coaxial);
    }();
    check(oneMate.error.empty() && oneMate.components[1].translations == 1 && oneMate.components[1].rotations == 1,
        "Coaxial mate removed the wrong degrees of freedom");
    const auto result = document.addAssembly(assembly);
    document.setPartProperties(result, {"G3D-016", "Flange and bushing", "", "Stage 16"});
    constexpr double pi = std::numbers::pi;
    verifySolids(document.find(result)->shape, {{{0, 0, 5}, 5250*pi}, {{0, 0, 20}, 1680*pi}});
    check(geom::checkInterference(document.find(result)->shape).empty(), "Face contact was treated as intersection");
    io::saveProject(assemblyPath, document);

    auto restored = io::openProject(assemblyPath);
    check(!io::updateLinkedComponents(restored, assemblyPath), "Unchanged files created an update");
    check(std::get<doc::Assembly>(restored.find(result)->parameters).mates == assembly.mates,
        "Mates were lost when reopening the assembly");
    const auto record = restored.componentRecord(result);
    check(record.children.size() == 2 && record.children[0].properties.designation == "G3D-016-01"
        && record.children[1].properties.material == "Bronze", "Component properties were lost");
    const auto cyclePath = folder.filePath("cyclic.graph3d");
    doc::Document cycle;
    cycle.addLinkedComponent(io::readLinkedComponent(assemblyPath, result));
    io::saveProject(cyclePath, cycle);
    bool cycleRejected = false;
    try {
        io::readLinkedComponent(cyclePath, 1, assemblyPath);
    } catch (const std::exception&) {
        cycleRejected = true;
    }
    check(cycleRejected, "Circular file references were allowed");
    const auto topPath = folder.filePath("nested-assembly.graph3d");
    doc::Document nested;
    const auto subgroup = nested.addLinkedComponent(io::readLinkedComponent(assemblyPath, result, topPath));
    const auto top = nested.addAssembly({{{subgroup, {}, true}, {subgroup, {0, {85, 0, 0}, {}}}}});
    io::saveProject(topPath, nested);

    check(flange.editExtrude(flangeBody, 12), "Could not edit flange thickness");
    flange.setPartProperties(flangeBody, {"G3D-016-01", "Flange", "Stainless steel", "Base"});
    io::saveProject(flangePath, flange);
    auto topDocument = io::openProject(topPath);
    check(io::updateLinkedComponents(topDocument, topPath), "Nested component did not update");
    verifySolids(topDocument.find(top)->shape,
        {{{0, 0, 6}, 6300*pi}, {{0, 0, 22}, 1680*pi}, {{85, 0, 6}, 6300*pi}, {{85, 0, 22}, 1680*pi}});
    check(topDocument.componentRecord(top).children[1].children[0].properties.material == "Stainless steel",
        "Nested metadata did not update");
    check(topDocument.undo(), "Linked refresh was not undoable");
    verifySolids(topDocument.find(top)->shape,
        {{{0, 0, 5}, 5250*pi}, {{0, 0, 20}, 1680*pi}, {{85, 0, 5}, 5250*pi}, {{85, 0, 20}, 1680*pi}});
    check(topDocument.redo(), "Linked refresh was not redoable");
    io::saveProject(topPath, topDocument);

    check(io::updateLinkedComponents(restored, assemblyPath), "Flange did not update");
    verifySolids(restored.find(result)->shape, {{{0, 0, 6}, 6300*pi}, {{0, 0, 22}, 1680*pi}});
    auto conflict = assembly;
    conflict.mates.push_back({2, 1, surface(moving, MateKind::Plane, 1), surface(fixed, MateKind::Plane, 1), false});
    const auto diagnosis = restored.assemblySolution(conflict);
    check(diagnosis.failedMate == 2 && !diagnosis.error.empty(), "Conflicting coincident planes were not identified");
    check(restored.editAssembly(result, conflict) && restored.find(result)->status == doc::Status::Failed,
        "A conflict did not mark the assembly as failed");
    check(restored.undo() && restored.find(result)->status == doc::Status::Ready, "Undo did not repair the assembly");

    io::saveProject(assemblyPath, restored);
    const auto assembledStep = folder.filePath("flange-and-bushing.step");
    io::exportVisible(assembledStep, restored, io::ExchangeFormat::Step);
    verifySolids(io::readStep(assembledStep), {{{0, 0, 6}, 6300*pi}, {{0, 0, 22}, 1680*pi}});
    auto colliding = assembly;
    colliding.mates[1].moving = findSurface(restored.find(moving)->shape, MateKind::Plane, 1);
    colliding.mates[1].opposite = false;
    colliding.mates[1].reference = findSurface(restored.find(fixed)->shape, MateKind::Plane, 1);
    // Enlarge the bushing OD so a buried placement has positive overlap.
    check(bushing.editSketch(radial, {geom::Plane::XZ, {geom::Rectangle{{4, 0}, {13, 20}}}}),
        "Could not make a collision test bushing");
    io::saveProject(folder.filePath("parts/bushing-oversize.graph3d"), bushing);
    check(restored.refreshLinkedComponents({{moving, io::readLinkedComponent(
        folder.filePath("parts/bushing-oversize.graph3d"), bushingBody, assemblyPath)}}), "Could not replace the test component");
    check(restored.find(result)->status == doc::Status::Failed, "Ambiguous cylindrical reference was not detected");
    colliding.mates[0].moving = findSurface(restored.find(moving)->shape, MateKind::Cylinder, 13);
    check(restored.editAssembly(result, colliding) && restored.find(result)->status == doc::Status::Ready,
        "Could not place the oversized bushing");
    const auto clashes = geom::checkInterference(restored.find(result)->shape);
    check(clashes.size() == 1 && clashes[0].first == std::vector<int>{1} && clashes[0].second == std::vector<int>{2}
        && std::abs(clashes[0].volumeMm3 - 828*pi) < 1.e-4, "Assembly overlap or its exact volume was not found");
    io::saveProject(folder.filePath("assembly-interference.graph3d"), restored);

    // Reopen with a missing source, then verify failed refresh preserves the cached model.
    const auto missing = folder.filePath("parts/missing-flange.graph3d");
    check(QFile::rename(flangePath, missing), "Could not move the test component");
    try {
        auto offline = io::openProject(assemblyPath);
        const auto revision = offline.revision();
        bool missingRejected = false;
        try {
            io::updateLinkedComponents(offline, assemblyPath);
        } catch (const std::exception&) {
            missingRejected = true;
        }
        check(missingRejected && offline.revision() == revision && offline.find(result)->status == doc::Status::Ready,
            "Missing source update destroyed the snapshot");
    } catch (...) {
        QFile::rename(missing, flangePath);
        throw;
    }
    check(QFile::rename(missing, flangePath), "Could not restore the test component");
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

bool closeTo(double a, double b)
{
    return std::abs(a - b) < 1.e-5;
}

void motionConstraints()
{
    using enum geom::MateKind;
    using enum geom::MateMode;
    doc::Document model;
    const auto base = block(model, 18, 12, 8);
    const auto moving = block(model, 10, 8, 4);
    const auto& ground = model.find(base)->shape;
    const auto& part = model.find(moving)->shape;
    const auto up = findSurface(ground, Plane, 1);
    const auto down = findSurface(part, Plane, -1);
    const auto movingUp = findSurface(part, Plane, 1);
    const auto side = [](const auto& shape) {
        const auto options = geom::assemblySurfaces(shape);
        return *std::ranges::find_if(options, [](const auto& p) { return p.kind == Plane && p.direction[0] > .999; });
    };
    doc::Assembly move{{{base, {}, true}, {moving, {0, {3, 2, 27}, {}}}},
        {{2, 1, down, up, true, Distance, 5}}};
    auto solved = model.assemblySolution(move);
    check(solved.error.empty() && solved.components[1].translations == 2
        && closeTo(solved.components[1].transform.TranslationPart().Z(), 13)
        && closeTo(solved.components[1].transform.TranslationPart().X(), 3), "Distance mate or free movement is incorrect");
    move.mates = {{2, 1, down, up, true, Travel, 2, 1, 7}};
    solved = model.assemblySolution(move);
    check(solved.error.empty() && closeTo(solved.components[1].transform.TranslationPart().Z(), 15)
        && closeTo(solved.components[1].transform.TranslationPart().Y(), 2), "Travel limit did not clamp a free coordinate");
    move.mates = {{2, 1, movingUp, up, false, Angle, 30}};
    solved = model.assemblySolution(move);
    const auto tilted = gp_Vec(0, 0, 1).Transformed(solved.components.at(1).transform);
    check(solved.error.empty() && solved.components[1].rotations == 2
        && closeTo(tilted.Angle(gp_Vec(0, 0, 1)), std::numbers::pi / 6), "Angle mate did not set the requested tilt");
    move.mates = {{2, 1, movingUp, up, false}, {2, 1, side(part), side(ground), false, Angle, 60}};
    solved = model.assemblySolution(move);
    check(solved.error.empty() && solved.components[1].rotations == 0
        && closeTo(gp_Vec(1, 0, 0).Transformed(solved.components[1].transform).Angle(gp_Vec(1, 0, 0)),
                   std::numbers::pi / 3), "Angle mate did not keep the prior planar alignment");
}

void screwDrive(const QDir& folder)
{
    using enum geom::MateKind;
    using enum geom::MateMode;
    doc::Document model;
    const auto tube = [&](double radius, double bore, double z, double length) {
        geom::Sketch sketch{geom::Plane::XY, {geom::Circle{{0, 0}, radius}}};
        if (bore > 0) { sketch.entities.push_back(geom::Circle{{0, 0}, bore}); }
        if (z != 0) { sketch.datumPlane = model.addAdvanced(geom::DatumPlane{{0, 0, z}, {0, 0, 1}}); }
        return model.addExtrude(model.addSketch(sketch), length);
    };
    const auto bearing = tube(14, 4.5, 0, 8);
    const auto screw = tube(4, 0, 0, 60);
    const auto carriage = tube(8, 4.25, 12, 8);
    const auto stop = block(model, 5, 3, 4);
    const auto bearingAxis = findSurface(model.find(bearing)->shape, Cylinder, 4.5);
    const auto screwAxis = findSurface(model.find(screw)->shape, Cylinder, 4);
    const auto carriageAxis = findSurface(model.find(carriage)->shape, Cylinder, 4.25);
    doc::Assembly setup{{{bearing, {}, true}, {screw, {}}, {carriage, {}}, {stop, {0, {5, 0, 27}, {}}, true}},
        {{2, 1, screwAxis, bearingAxis, false},
         {2, 1, findSurface(model.find(screw)->shape, Plane, -1), findSurface(model.find(bearing)->shape, Plane, -1), false},
         {3, 2, carriageAxis, screwAxis, false, Screw, 10, 0, 20}}};
    setup.driveAngle = 450;
    const auto state = model.assemblySolution(setup);
    check(state.error.empty() && closeTo(state.screwTravel, 12.5) && closeTo(state.components.at(2).transform.TranslationPart().Z(), 12.5),
          "The carriage travel did not equal the screw lead per turn");
    const auto arm = gp_Vec(1, 0, 0).Transformed(state.components[1].transform);
    check(closeTo(arm.X(), 0) && closeTo(arm.Y(), 1), "The screw did not actually rotate");
    const auto guide = gp_Vec(1, 0, 0).Transformed(state.components[2].transform);
    check(closeTo(guide.X(), 1) && closeTo(guide.Y(), 0), "The carriage inherited screw rotation");
    setup.driveAngle = 1080;
    const auto end = model.assemblySolution(setup);
    check(end.error.empty() && closeTo(end.driveAngle, 720) && closeTo(end.screwTravel, 20)
        && closeTo(end.components.at(2).transform.TranslationPart().Z(), 20), "Screw end stop did not clamp both components");
    setup.driveAngle = -360;
    check(closeTo(model.assemblySolution(setup).screwTravel, 0), "Screw lower stop failed");
    const auto stations = model.scanAssembly(setup, 0, 720, 3);
    check(stations.size() == 3 && stations.front().pairs.empty() && stations.back().pairs.empty()
        && stations[1].pairs.size() == 1 && stations[1].pairs.front().first == std::vector<int>{3}
        && stations[1].pairs.front().second == std::vector<int>{4}, "Collision scan missed the obstacle mid-travel");
    setup.driveAngle = 360;
    auto reversed = setup;
    reversed.mates.back().value = -10;
    reversed.driveAngle = -360;
    check(closeTo(model.assemblySolution(reversed).screwTravel, 10), "Left-hand screw did not reverse travel");
    setup.components[2].simplified = true;
    const auto object = model.addAssembly(setup);
    check(!model.find(object)->preview.IsNull()
        && geom::checkInterference(model.find(object)->shape).size() == 1, "The simplified view altered precise collision geometry");
    TopoDS_Iterator exact(model.find(object)->shape), preview(model.find(object)->preview);
    exact.Next(); exact.Next();
    preview.Next(); preview.Next();
    GProp_GProps precise, proxy;
    BRepGProp::VolumeProperties(exact.Value(), precise);
    BRepGProp::VolumeProperties(preview.Value(), proxy);
    check(proxy.Mass() > precise.Mass() && proxy.CentreOfMass().Distance(precise.CentreOfMass()) < 1.e-5,
        "Simplified component did not keep its position");
    const auto file = folder.filePath("screw-drive.graph3d");
    io::saveProject(file, model);
    const auto restored = io::openProject(file);
    const auto& persisted = std::get<doc::Assembly>(restored.find(object)->parameters);
    check(persisted.components == setup.components && persisted.mates == setup.mates && persisted.driveAngle == 360
        && !restored.find(object)->preview.IsNull(), "Screw drive or simplified instance was lost on save");
}

void contextualAssembly(const QDir& folder)
{
    doc::Document model;
    const auto top = block(model, 32, 20, 14);
    const auto component = block(model, 10, 8, 5);
    doc::Assembly assembly{{{top, {}, true}, {component, {0, {40, 0, 0}, {}}, false, true,
        geom::ContextFaceEdit{1, {{5, 4, 5}, {0, 0, 1}}, {{16, 10, 14}, {0, 0, 1}}, -2}}}};
    const auto id = model.addAssembly(assembly);
    verifySolids(model.find(id)->shape, {{{16, 10, 7}, 8960}, {{45, 4, 6}, 960}});
    verifySolids(model.find(component)->shape, {{{5, 4, 2.5}, 400}});
    check(model.editExtrude(top, 18), "Could not modify contextual reference");
    verifySolids(model.find(id)->shape, {{{16, 10, 9}, 11520}, {{45, 4, 8}, 1280}});
    const auto file = folder.filePath("context.graph3d");
    io::saveProject(file, model);
    auto reopened = io::openProject(file);
    check(std::get<doc::Assembly>(reopened.find(id)->parameters).components == assembly.components,
        "Contextual relationship or identity did not survive reopening");
    verifySolids(reopened.find(id)->shape, {{{16, 10, 9}, 11520}, {{45, 4, 8}, 1280}});
    const auto profile = std::get<doc::Extrude>(reopened.find(top)->parameters).profile;
    check(reopened.editSketch(profile, {geom::Plane::XY, {geom::Rectangle{{0, 0}, {8, 8}}}})
        && reopened.find(id)->status == doc::Status::Failed
        && reopened.find(id)->error.find("Contextual reference") != std::string::npos, "Lost contextual surface was not diagnosed");
    check(reopened.undo() && reopened.find(id)->status == doc::Status::Ready, "Undo did not restore contextual geometry");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        assemblyProject();
        coincidentComponents();
        doc::Document cylinder;
        const auto sk = cylinder.addSketch({geom::Plane::XY, {geom::Circle{{0, 0}, 10}}});
        const auto body = cylinder.addExtrude(sk, 20);
        check(geom::assemblySurfaces(cylinder.find(body)->shape).size() == 3, "Extruded cylinder mating surfaces missing");
        QTemporaryDir output;
        const auto args = QCoreApplication::arguments();
        const QDir folder(args.size() == 2 ? args.at(1) : output.path());
        check(folder.exists(), "Assembly test output directory is unavailable");
        linkedAndMatedAssembly(folder);
        motionConstraints();
        screwDrive(folder);
        contextualAssembly(folder);
    } catch (const Standard_Failure& error) {
        std::cerr << error.GetMessageString() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
