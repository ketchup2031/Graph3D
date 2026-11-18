#include "io/DraftIO.h"
#include "io/Exchange.h"
#include "io/ModelViewsIO.h"
#include "io/ProjectIO.h"

#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Bnd_Box.hxx>
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

namespace doc = graph3d::document;
namespace geom = graph3d::geometry;
namespace draw = graph3d::drawing;
namespace io = graph3d::io;
using Kind = geom::SurfaceKind;

namespace {

void check(bool condition, const char* description)
{
    if (!condition) { throw std::runtime_error(description); }
}

bool closeEnough(double actual, double expected, double tolerance = 1.e-5)
{
    return std::abs(actual - expected) < tolerance;
}

geom::SpatialCurve rectangle(double z)
{
    return {{{0,0,z}, {60,0,z}, {60,40,z}, {0,40,z}}, false, true};
}

const TopoDS_Shape& shape(const doc::Document& document, int id)
{
    const auto* object = document.find(id);
    if (!object || object->status != doc::Status::Ready || object->shape.IsNull()) {
        throw std::runtime_error("Missing rebuilt shape #" + std::to_string(id) + (object ? " " + object->error : ""));
    }
    return object->shape;
}

void surfaces(const QDir& directory)
{
    doc::Document cover;
    const auto first = cover.addAdvanced(rectangle(0));
    const auto upper = cover.addAdvanced(rectangle(30));
    const auto skin = cover.addAdvanced(doc::Surface{Kind::Loft, {first, upper}});
    check(TopExp_Explorer(shape(cover, skin), TopAbs_FACE).More()
        && !TopExp_Explorer(shape(cover, skin), TopAbs_SOLID).More(), "Loft is not an open surface");
    const auto free = cover.addAdvanced(doc::Surface{Kind::OpenEdges, {skin}});
    int loops = 0;
    for (TopExp_Explorer wires(shape(cover, free), TopAbs_WIRE); wires.More(); wires.Next()) { ++loops; }
    check(loops == 2, "Loft lost its two open boundaries");
    cover.setVisible(free, false);
    const auto bottom = cover.addAdvanced(doc::Surface{Kind::Patch, {skin}, .001, 1});
    const auto top = cover.addAdvanced(doc::Surface{Kind::Patch, {skin}, .001, 2});
    const auto shell = cover.addAdvanced(doc::Surface{Kind::Sew, {skin, bottom, top}});
    check(!doc::isBody(cover.find(shell)->parameters), "Sewing silently made a solid");
    const auto solid = cover.addAdvanced(doc::Surface{Kind::Solid, {shell}});
    check(BRepCheck_Analyzer(shape(cover,solid)).IsValid()
        && closeEnough(geom::massProperties(shape(cover,solid),0).volumeMm3,72000), "Invalid closed cover");
    const auto native = directory.filePath("cover.graph3d");
    io::saveProject(native, cover);
    cover = io::openProject(native);
    check(cover.editAdvanced(upper, rectangle(45))
        && closeEnough(geom::massProperties(shape(cover,solid),0).volumeMm3,108000), "Edited section did not rebuild the cover");
    check(cover.undo() && closeEnough(geom::massProperties(shape(cover,solid),0).volumeMm3,72000), "Cover edit undo failed");
    cover.setVisible(solid, false);
    cover.setVisible(skin, true);
    const auto step = directory.filePath("open-skin.step");
    io::exportVisible(step, cover, io::ExchangeFormat::Step);
    doc::Document imported;
    const auto importedId = imported.importTree(io::readStepTree(step));
    check(doc::isSurface(imported.find(importedId)->parameters)
        && TopExp_Explorer(shape(imported,importedId),TopAbs_FACE).More()
        && !TopExp_Explorer(shape(imported,importedId),TopAbs_SOLID).More(), "Open STEP became a solid");
    const auto savedImport = directory.filePath("imported-skin.graph3d");
    io::saveProject(savedImport, imported);
    check(doc::isSurface(io::openProject(savedImport).find(importedId)->parameters), "Imported surface did not persist");

    std::clog << "Checking sweep" << std::endl;
    const auto line = geom::makeSpatialCurve({{{0,0,0},{30,0,0}}});
    const auto path = geom::makeSpatialCurve({{{0,0,0},{0,0,25},{0,10,50}}, true});
    const std::array sweepInput{line, path};
    check(TopExp_Explorer(geom::buildSurface(Kind::Sweep, sweepInput, .001, 0), TopAbs_FACE).More(), "Surface sweep failed");
    const auto perimeter = geom::makeSpatialCurve(rectangle(0));
    std::clog << "Checking trim" << std::endl;
    const std::array patchInput{perimeter};
    const auto face = geom::buildSurface(Kind::Patch, patchInput, .001, 1);
    const auto dividing = geom::makeSpatialCurve({{{30,-10,-10},{30,50,-10},{30,50,10},{30,-10,10}}, false, true});
    const std::array dividerInput{dividing};
    const auto divider = geom::buildSurface(Kind::Patch, dividerInput, .001, 1);
    const std::array trimInput{face, divider};
    const auto half = geom::buildSurface(Kind::Trim, trimInput, 0, 1);
    check(TopExp_Explorer(half, TopAbs_FACE).More(), "Trimming did not return a face");
    std::clog << "Checking extension" << std::endl;
    const std::array extensionInput{face};
    const auto extended = geom::buildSurface(Kind::Extend, extensionInput, 10, 2);
    Bnd_Box prior, after;
    BRepBndLib::AddOptimal(face, prior, false, false);
    BRepBndLib::AddOptimal(extended, after, false, false);
    const auto extent = [](const Bnd_Box& box) {
        return box.CornerMin().Distance(box.CornerMax());
    };
    check(extent(after) > extent(prior), "Extension did not grow the surface");
}

void curveNetwork()
{
    const auto perimeter = geom::makeSpatialCurve(rectangle(0));
    const auto guide = geom::makeSpatialCurve({{{4,20,0},{30,20,6},{56,20,0}}, true});
    const std::array networkInput{perimeter, guide};
    const auto result = geom::buildSurface(Kind::Network, networkInput, .001, 1);
    Bnd_Box bounds;
    BRepBndLib::Add(result, bounds, false);
    check(TopExp_Explorer(result, TopAbs_FACE).More() && bounds.CornerMax().Z() > 5.,
        "Curve network did not follow the spatial guide");
    const auto warped = geom::makeSpatialCurve({{{0,0,0},{40,0,12},{40,30,0},{0,30,8}}, false, true});
    const std::array warpedInput{warped};
    check(BRepCheck_Analyzer(geom::buildSurface(Kind::Patch, warpedInput, .001, 1)).IsValid(),
        "Nonplanar boundary could not be patched");
    const std::array openInput{guide};
    bool rejected = false;
    try { geom::buildSurface(Kind::Network, openInput, .001, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "An open network boundary was accepted");
}

void materialsAndAnnotations(const QDir& directory)
{
    doc::Document model;
    const auto housing = model.addPrimitive(geom::Primitive::Cube);
    const auto stock = model.addPrimitive(geom::Primitive::Cylinder);
    const auto stockFaces = geom::planarFaces(shape(model,stock));
    const auto stockTop = std::ranges::find_if(stockFaces, [](const auto& face) { return face.outward[2] > .99; });
    check(stockTop != stockFaces.end(), "Missing bushing face");
    const auto bushing = model.addHole(stock,*stockTop,{30,100,true});
    model.setPartProperties(housing, {"HS", "Housing"});
    model.setPartProperties(bushing, {"BS", "Bushing"});
    check(model.setCatalogMaterial(housing, "Carbon steel") && model.setCatalogMaterial(bushing, "Aluminium 6061"),
          "Could not assign catalog materials");
    geom::Placement placed;
    placed.offset = {30,0,0};
    const auto assembly = model.addAssembly({{{housing,{}},{bushing,placed}}});
    const auto result = model.measure(assembly);
    const double bushingMass = std::numbers::pi * (50*50 - 15*15) * 100 * 2700 / 1.e9;
    const double mass = 7.85 + bushingMass;
    const double center = (7.85 * 50 + bushingMass * 220) / mass;
    check(result.massKg && closeEnough(*result.massKg, mass) && result.centerMm && closeEnough((*result.centerMm)[0], center)
        && closeEnough((*result.centerMm)[1],50) && result.inertiaKgM2, "Assembly used uniform rather than component densities");
    const auto xx = 7.85 * .01 / 6 + bushingMass * (3*(.05*.05 + .015*.015) + .1*.1) / 12;
    const auto yy = xx + 7.85 * std::pow((50-center)/1000,2) + bushingMass * std::pow((220-center)/1000,2);
    check(closeEnough((*result.inertiaKgM2)[0], xx) && closeEnough((*result.inertiaKgM2)[1], yy),
        "Assembly inertia does not match the parallel-axis calculation");

    const auto faces = geom::planarFaces(shape(model,housing));
    const auto top = std::ranges::find_if(faces, [](const auto& face) { return face.outward[2] > .99; });
    check(top != faces.end(), "Missing drilling face");
    const auto hole = model.addHole(housing,*top,{16,100,true});
    check(model.measure(hole).massKg.has_value(), "Machining lost the assigned density");
    const auto linear = model.addAnnotation({doc::AnnotationKind::Linear, housing, 1,2});
    const auto diameter = model.addAnnotation({doc::AnnotationKind::Diameter, hole});
    const auto datum = model.addAnnotation({doc::AnnotationKind::Datum, housing, 1, 2, faces.front(), "A"});
    const auto rough = model.addAnnotation({doc::AnnotationKind::Roughness, housing, 1, 2, faces.front(), "3.2"});
    for (const auto id : {linear,diameter,datum,rough}) {
        check(model.resolveAnnotation(id).has_value() && model.annotationBelongsTo(id,hole), "3D notation did not resolve");
    }
    model.setVisible(assembly, false);
    const auto source = directory.filePath("annotated-housing.graph3d");
    io::saveProject(source,model);
    auto loaded = io::openModelSource(source);
    check(closeEnough(*loaded.document.measure(assembly).massKg,mass), "Material assignment did not persist");
    draw::Draft draft;
    auto views = io::buildModelViews(loaded, hole, {}, draw::draftSize(draft));
    views.annotationSelection = {{linear,0},{diameter,2},{datum,1},{rough,1}};
    draft.modelViews = io::buildModelViews(loaded, hole, {}, draw::draftSize(draft), &views);
    check(draft.modelViews->annotations.size() == 4 && draft.modelViews->annotations[1].faceOn
        && closeEnough(draft.modelViews->annotations[1].value,16), "Model notation did not transfer");
    const auto drawing = directory.filePath("annotated-housing.g3draw");
    io::saveDraft(drawing,draft);
    auto reopened = io::openDraft(drawing);
    check(reopened.modelViews->annotations.size() == 4 && reopened.modelViews->annotationSelection == views.annotationSelection,
        "Transferred annotations did not survive reopening");
    check(model.editHole(hole,{24,100,true}) && closeEnough(model.resolveAnnotation(diameter)->value,24), "3D diameter did not update");
    io::saveProject(source,model);
    loaded = io::openModelSource(source);
    reopened.modelViews = io::buildModelViews(loaded,hole,{},draw::draftSize(reopened),&*reopened.modelViews);
    check(closeEnough(reopened.modelViews->annotations[1].value,24)
        && reopened.modelViews->annotations[1].sourceId == diameter, "Drawing diameter did not update");
    io::saveDraft(drawing,reopened);
    draw::exportDraftPdf(directory.filePath("annotated-housing.pdf"),reopened);
}

}  // namespace

int main(int argc,char** argv)
{
    QGuiApplication app(argc,argv);
    const QTemporaryDir temp;
    const QDir directory(QDir(app.arguments().size() > 1 ? app.arguments()[1] : temp.path()).absolutePath());
    try {
        check(directory.exists(), "Missing output directory");
        surfaces(directory);
        std::clog << "Checking materials and annotations" << std::endl;
        materialsAndAnnotations(directory);
        std::clog << "Checking network" << std::endl;
        curveNetwork();
    } catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString();
        std::cerr << error.DynamicType()->Name() << ": " << (message ? message : "") << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
