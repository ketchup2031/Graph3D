#include "io/DraftIO.h"
#include "io/DxfIO.h"
#include "io/Exchange.h"
#include "io/FlatDrawing.h"
#include "io/ProjectIO.h"

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>

#include <QDir>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace doc = graph3d::document;
namespace geom = graph3d::geometry;
namespace io = graph3d::io;
using namespace graph3d::drawing;

void check(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

bool near(double a, double b)
{
    return std::abs(a - b) < 1.e-5;
}

void cadDrawing(const QDir& directory)
{
    Draft draft;
    draft.title = "Cover";
    draft.layers = {{"0"}, {"MACHINING"}};
    DraftItem fragment{DraftKind::Fragment, {60,65}};
    fragment.id = QUuid::createUuid();
    fragment.layer = 1;
    fragment.fragment.push_back({DraftKind::Circle, {0,0}, {7,0}});
    fragment.fragment.push_back({DraftKind::Line, {-10,0}, {10,0}});
    draft.items.push_back(fragment);
    draft.items.push_back({DraftKind::Horizontal, {40,40}, {80,40}, {60,30}});
    draft.items.back().id = QUuid::createUuid();
    draft.additionalSheets.emplace_back();
    auto& second = draft.additionalSheets.back();
    second.title = "Detail";
    second.items.push_back({DraftKind::Vertical, {40,40}, {40,75}, {50,57}});
    second.items.back().id = QUuid::createUuid();
    draft.additionalSheets.emplace_back();
    draft.additionalSheets.back().title = "Blank";
    const auto checkDrawing = [](const Draft& result, bool multiSheet) {
        const auto hasBlock = [](const auto& items) {
            return std::ranges::any_of(items, [](const auto& item) {
                return item.kind == DraftKind::Fragment && item.fragment.size() == 2;
            });
        };
        check(hasBlock(result.items), "Block entities did not round trip");
        const auto dimension = std::ranges::find(result.items, DraftKind::Horizontal, &DraftItem::kind);
        check(dimension != result.items.end() && near(QLineF(dimension->a, dimension->b).length(), 40),
              "Editable DXF dimension changed");
        check(std::ranges::any_of(result.layers, [](const auto& layer) { return layer.name == "MACHINING"; }),
              "DXF layer was lost");
        if (multiSheet) {
            check(result.additionalSheets.size() == 2 && result.additionalSheets[0].title == "Detail"
                && result.additionalSheets[0].items.size() == 1
                && result.additionalSheets[0].items[0].kind == DraftKind::Vertical
                && result.additionalSheets[1].title == "Blank" && result.additionalSheets[1].items.empty(),
                  "DXF sheet was lost");
        }
    };
    const auto dxf = directory.filePath("multisheet.dxf");
    check(io::exportDxfReport(dxf, draft, true).isEmpty(), "Supported DXF content was skipped");
    const auto loaded = io::importDxf(dxf);
    check(loaded.losses.isEmpty(), "DXF import reported losses");
    checkDrawing(loaded.drawing, true);
    if (directory.exists("multisheet-external.dxf")) {
        checkDrawing(io::importDxf(directory.filePath("multisheet-external.dxf")).drawing, true);
    }
    DraftItem spline{DraftKind::Spline};
    spline.points = {{20,30},{25,50},{35,50},{40,30}};
    draft.items.push_back(spline);
    const auto loss = io::exportDxfReport(directory.filePath("checked-loss.dxf"),draft);
    check(loss.value("SPLINE") == 1, "DXF report did not identify the skipped entity");
    draft.items.pop_back();
    if (qEnvironmentVariableIsSet("GRAPH3D_LIBREDWG_DIR")) {
        const auto dwg = directory.filePath("drawing.dwg");
        draft.additionalSheets.clear();
        io::exportDxf(directory.filePath("single-sheet.dxf"),draft);
        check(io::exportDwg(dwg, draft).isEmpty(), "Supported content was skipped in DWG export");
        checkDrawing(io::importDwg(dwg).drawing, false);
    }
}

void cadModels(const QDir& directory)
{
    doc::Document source;
    const auto cube = source.addPrimitive(geom::Primitive::Cube);
    const auto cylinder = source.addPrimitive(geom::Primitive::Cylinder);
    check(source.setPartProperties(cube, {"", "Housing", "", "", {{.8,.1,.2}}}), "Set housing color");
    check(source.setPartProperties(cylinder, {"", "Shaft", "", "", {{.1,.2,.9}}}), "Set shaft color");
    const auto subassembly = source.addAssembly({{{cylinder,{}}}});
    check(source.setPartProperties(subassembly, {"", "Bearing group"}), "Set subassembly name");
    geom::Placement placement;
    placement.offset = {150,0,0};
    const auto top = source.addAssembly({{{cube,{}},{subassembly,placement}}});
    check(source.setPartProperties(top, {"", "Test assembly"}), "Set assembly name");
    const auto initial = directory.filePath("assembly.step");
    io::exportVisible(initial,source,io::ExchangeFormat::Step);
    const auto validate = [](const QString& path) {
        auto roots = io::readStepTree(path);
        check(roots.size() == 1 && roots[0].properties.name == "Test assembly"
            && roots[0].children.size() == 2, "STEP assembly name or components changed");
        const auto& housing = roots[0].children[0];
        const auto& bearing = roots[0].children[1];
        check(housing.properties.name == "Housing" && housing.properties.color && near((*housing.properties.color)[0],.8),
              "STEP housing metadata changed");
        check(bearing.properties.name == "Bearing group" && bearing.children.size() == 1, "STEP nested structure changed");
        const auto& shaft = bearing.children[0];
        check(shaft.properties.name == "Shaft" && shaft.properties.color && near((*shaft.properties.color)[2],.9),
              "STEP shaft metadata changed");
        Bnd_Box bounds;
        BRepBndLib::AddOptimal(housing.shape,bounds,false,false);
        check(near(bounds.CornerMax().X() - bounds.CornerMin().X(),100), "STEP size changed");
        Bnd_Box placed;
        BRepBndLib::AddOptimal(shaft.shape,placed,false,false);
        check(placed.CornerMin().X() > 95, "STEP lost component placement");
        return roots;
    };
    if (directory.exists("freecad-roundtrip.step")) {
        validate(directory.filePath("freecad-roundtrip.step"));
    }
    doc::Document restored;
    restored.importTree(validate(initial));
    const auto saved = directory.filePath("imported.graph3d");
    io::saveProject(saved,restored);
    const auto repeated = directory.filePath("assembly-reexport.step");
    io::exportVisible(repeated,io::openProject(saved),io::ExchangeFormat::Step);
    validate(repeated);

    doc::Document part;
    part.addPrimitive(geom::Primitive::Cube);
    const auto iges = directory.filePath("cube.iges");
    io::exportVisible(iges,part,io::ExchangeFormat::Iges);
    Bnd_Box bounds;
    BRepBndLib::AddOptimal(io::readIges(iges),bounds,false,false);
    check(near(bounds.CornerMax().Z() - bounds.CornerMin().Z(),100), "IGES units changed");
    const auto stl = directory.filePath("cube.stl");
    io::exportVisible(stl,part,io::ExchangeFormat::Stl);
    doc::Document mesh;
    const auto meshId = mesh.addImportedMesh(io::readStl(stl,25.4));
    const auto path = directory.filePath("mesh.graph3d");
    io::saveProject(path,mesh);
    const auto reopened = io::openProject(path);
    const auto& triangles = std::get<doc::ImportedMesh>(reopened.find(meshId)->parameters).vertices;
    check(triangles.size() == 36 && near(std::ranges::max(triangles)[0],2540)
        && !doc::isBody(reopened.find(meshId)->parameters)
        && reopened.find(meshId)->status == doc::Status::Ready, "STL mesh or scale did not round trip");
}

double volume(const TopoDS_Shape& solid)
{
    GProp_GProps properties;
    BRepGProp::VolumeProperties(solid,properties);
    return properties.Mass();
}

void sheetMetal(const QDir& directory)
{
    geom::SheetMetal bracket{60,40,1.5,{{true,20,2,90,.42,1},{false,25,2,90,.42,1}},{{2,12,18,6}}};
    const auto flat = geom::flattenSheet(bracket);
    check(flat.bends.size() == 2 && flat.holes.size() == 1
        && near(flat.length,105 + std::numbers::pi*(2 + .42*1.5)), "Incorrect development length");
    const auto folded = geom::makeSheetMetal(bracket);
    auto blank = bracket;
    blank.holes.clear();
    TopExp_Explorer solids(folded,TopAbs_SOLID);
    check(solids.More(), "Sheet did not create a solid");
    solids.Next();
    check(!solids.More() && near(volume(geom::makeSheetMetal(blank)) - volume(folded), 9*std::numbers::pi*1.5),
          "Sheet hole is not a through hole on a single folded solid");
    doc::Document project;
    const auto id = project.addSheetMetal(bracket);
    const auto source = directory.filePath("bracket.graph3d");
    io::saveProject(source,project);
    check(std::get<geom::SheetMetal>(io::openProject(source).find(id)->parameters) == bracket, "Sheet inputs changed");
    auto draft = io::flatDrawing(bracket);
    draft.flatPattern = FlatPatternReference{source,id,{}};
    for (const auto& item : draft.items) { draft.flatPattern->geometry.push_back(item.id); }
    check(draft.items.size() == 4 && draft.items[0].points.front() == draft.items[0].points.back()
        && draft.items[1].kind == DraftKind::Circle && draft.items[2].layer == 1 && draft.items[3].layer == 1,
        "Wrong cut or bend geometry");
    check(io::exportDxf(directory.filePath("bracket-cut.dxf"),draft) == 0, "Flat pattern DXF is incomplete");
    DraftItem note{DraftKind::Text,{100,100}};
    note.text = "Shop note";
    note.id = QUuid::createUuid();
    note.layer = 2;
    draft.items.push_back(note);
    const auto path = directory.filePath("bracket.g3draw");
    io::saveDraft(path,draft);
    bracket.bends[0].flange += 10;
    check(project.editSheetMetal(id,bracket), "Sheet edit failed");
    io::saveProject(source,project);
    const auto refreshed = io::refreshFlatDrawing(io::openDraft(path));
    check(refreshed.items.size() == 5 && refreshed.items[0].text == "Shop note"
        && refreshed.paper.requirements.contains(QString::number(flat.length+10,'f',2)), "Flat pattern did not refresh");
    check(project.undo(), "Sheet edit cannot be undone");
    io::saveProject(source,project);
    bracket.holes.push_back(bracket.holes.front());
    bool rejected = false;
    try { geom::flattenSheet(bracket); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Overlapping sheet cuts were accepted");
    geom::SheetMetal collision;
    collision.bends = {{false,10,2,90},{false,30,2,90},{false,40,2,90}};
    rejected = false;
    try { geom::flattenSheet(collision); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Intersecting folded panels were accepted");
}

int main(int argc,char** argv)
{
    QGuiApplication app(argc,argv);
    const QTemporaryDir temp;
    const QDir requested(app.arguments().size() > 1 ? app.arguments()[1] : temp.path());
    const QDir directory(requested.absolutePath());
    std::string_view stage = "2D exchange";
    try {
        check(directory.exists(), "Missing output folder");
        cadDrawing(directory);
        stage = "3D exchange";
        cadModels(directory);
        stage = "sheet metal";
        sheetMetal(directory);
    } catch (const Standard_Failure& failure) {
        const auto* message = failure.GetMessageString();
        std::cerr << stage << ": " << failure.DynamicType()->Name() << ": " << (message ? message : "") << '\n';
        return 1;
    } catch (const std::exception& failure) {
        std::cerr << stage << ": " << failure.what() << '\n';
        return 1;
    }
    return 0;
}
