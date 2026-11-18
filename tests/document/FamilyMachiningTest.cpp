#include "document/Document.h"
#include "drawing/Draft.h"
#include "io/DraftIO.h"
#include "io/Exchange.h"
#include "io/ModelViewsIO.h"
#include "io/ProjectIO.h"

#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS.hxx>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

using namespace graph3d;
using namespace graph3d::geometry;
using namespace graph3d::document;

void check(bool passed, const char* message)
{
    if (!passed) { throw std::runtime_error(message); }
}

bool almostEqual(double a, double b, double eps = 1.e-4) { return std::abs(a - b) < eps; }

double volume(const TopoDS_Shape& solid)
{
    check(BRepCheck_Analyzer(solid).IsValid(), "body is not valid");
    GProp_GProps result;
    BRepGProp::VolumeProperties(solid, result);
    return result.Mass();
}

double height(const TopoDS_Shape& shape)
{
    Bnd_Box bounds;
    BRepBndLib::AddOptimal(shape, bounds, false, false);
    return bounds.CornerMax().Z() - bounds.CornerMin().Z();
}

void family(const QTemporaryDir& tmp)
{
    Document doc;
    Sketch ring;
    ring.entities = {Circle{{0, 0}, 15}, Circle{{0, 0}, 12}};
    ring.constraints = {{ConstraintKind::Diameter, {0, 0}, {}, 30}, {ConstraintKind::Diameter, {1, 0}, {}, 24}};
    const auto profile = doc.addSketch(ring);
    const auto part = doc.addExtrude(profile, 8);
    Configuration config;
    config.variables = {{"outer", "30"}, {"wall", "3"}, {"inner", "outer - 2*wall"}, {"height", "2^3"}};
    config.bindings = {{profile, {{"dimension.1", "outer"}, {"dimension.2", "inner"}}}, {part, {{"distance", "height"}}}};
    config.variants = {{"Small", {{"outer", "30"}}}, {"Large", {{"outer", "40"}}}};
    config.activeVariant = "Small";
    check(doc.setConfiguration(config), "cannot configure family");
    check(almostEqual(volume(doc.find(part)->shape), 8*std::numbers::pi*(225-144)), "wrong small bushing volume");
    check(doc.selectVariant("Large"), "cannot select large");
    check(almostEqual(volume(doc.find(part)->shape), 8*std::numbers::pi*(400-289)), "wrong large bushing volume");
    check(doc.undo() && doc.configuration().activeVariant == "Small", "variant undo failed");
    check(doc.redo() && doc.configuration().activeVariant == "Large", "variant redo failed");

    const auto file = tmp.filePath("bushings.graph3d");
    const auto drawingPath = tmp.filePath("bushings.g3draw");
    io::saveProject(file, doc);
    auto source = io::openModelSource(file);
    check(source.document.configuration().activeVariant == "Large", "active variant lost");
    drawing::Draft draft;
    draft.modelViews = io::buildModelViews(source, part, {}, {297, 210}, nullptr, QString("Small"));
    check(almostEqual(draft.modelViews->geometry->views.top.width, 30), "drawing did not use its pinned variant");
    io::saveDraft(drawingPath, draft);
    draft = io::openDraft(drawingPath);
    check(draft.modelViews->variant == QString("Small"), "drawing lost its pinned variant");
    auto updated = doc.configuration();
    updated.variants.at("Small").at("outer") = "32";
    doc.setConfiguration(updated);
    io::saveProject(file, doc);
    const auto model = io::buildModelViews(io::openModelSource(file), part, {}, {297, 210}, &*draft.modelViews);
    check(almostEqual(model.geometry->views.top.width, 32), "drawing failed to update its pinned variant");

    for (const auto& [formula, diagnostic] : {std::pair{"inner", "Cyclic variable dependency"},
            std::pair{"missing", "Unknown variable"}, std::pair{"3/0", "Division by zero"},
            std::pair{"-4", "Object #1"}}) {
        auto bad = updated;
        bad.variables["wall"] = formula;
        if (std::string(formula) == "-4") { bad.variables["inner"] = "wall"; }
        std::string message;
        try { doc.setConfiguration(std::move(bad)); }
        catch (const std::invalid_argument& error) { message = error.what(); }
        check(message.find(diagnostic) != std::string::npos, "expected distinct diagnostic");
        check(doc.configuration() == updated, "invalid formula altered the model");
    }
}

void holesAndPatterns(const QTemporaryDir& tmp)
{
    Document doc;
    const auto base = doc.addImported("plate", BRepPrimAPI_MakeBox(110, 80, 16).Shape());
    const FaceReference entry{{15, 25, 16}, {0, 0, 1}};
    HoleSize size{7, 16, true, HoleStyle::Counterbore, 12, 4, ThreadSize{8, 1.25, 8, true}};
    const auto hole = doc.addHole(base, entry, size);
    const auto pattern = doc.addAdvanced(FeaturePattern{hole, 4, {23, 0, 0}});
    const auto removed = 4 * std::numbers::pi * (3.5*3.5*16 + (6*6-3.5*3.5)*4);
    const auto plate = 110*80*16;
    check(almostEqual(plate - volume(doc.find(pattern)->shape), removed), "array did not cut four stepped holes");
    size.seat = 5;
    check(doc.editHole(hole, size), "cannot edit seed");
    check(almostEqual(plate - volume(doc.find(pattern)->shape), removed + 4*std::numbers::pi*(36-12.25)), "array did not rebuild after seed edit");

    const auto file = tmp.filePath("plate.graph3d");
    const auto drawingFile = tmp.filePath("plate.g3draw");
    io::saveProject(file, doc);
    const auto source = io::openModelSource(file);
    check(almostEqual(volume(source.document.find(pattern)->shape), volume(doc.find(pattern)->shape)), "array did not survive reopen");
    drawing::Draft draft;
    draft.modelViews = io::buildModelViews(source, pattern, {}, {297, 210});
    check(draft.modelViews->threads.size() == 4 && draft.modelViews->threads[0].designation.contains("M8×1.25 LH"),
          "thread callout missing from linked drawing");
    io::saveDraft(drawingFile, draft);
    check(io::openDraft(drawingFile).modelViews->threads.size() == 4, "thread callouts did not survive reopen");
    drawing::exportDraftPdf(tmp.filePath("threads.pdf"), draft);

    const auto sink = doc.addHole(base, {{25, 55, 16}, {0, 0, 1}}, {7, 10, false, HoleStyle::Countersink, 13, 90});
    check(volume(doc.find(sink)->shape) < plate, "countersink failed");

    for (const auto entity : {SketchEntity{Arc{{0, 0}, {28, 11}, {56, 0}}},
            SketchEntity{Spline{{{0, 0}, {22, 8}, {42, -4}, {57, 3}}}}}) {
        Sketch path;
        path.trajectory = true;
        path.entities = {entity};
        const auto route = doc.addSketch(path);
        const auto id = doc.addAdvanced(FeaturePattern{hole, 3, {}, route});
        const auto removedSeed = plate - volume(doc.find(hole)->shape);
        check(almostEqual(plate - volume(doc.find(id)->shape), 3*removedSeed), "curved path pattern has wrong volume");
    }
}

void directEdits(const QTemporaryDir& tmp)
{
    const auto box = BRepPrimAPI_MakeBox(70, 50, 20).Shape();
    const auto edges = twoFaceEdges(box);
    const auto edge = std::ranges::find_if(edges, [](const auto& e) {
        return almostEqual(e.samples.front()[0], e.samples.back()[0]) && almostEqual(e.samples.front()[1], e.samples.back()[1])
            && std::abs(e.samples.front()[2] - e.samples.back()[2]) > 19;
    });
    check(edge != edges.end(), "no vertical edge");
    const auto tapered = treatEdges(box, std::span(&*edge, 1), EdgeTreatmentKind::Fillet, 1.5, true, 3.5);
    check(volume(tapered) < volume(box), "variable radius did not cut material");
    const auto original = treatEdges(box, std::span(&*edge, 1), EdgeTreatmentKind::Fillet, 4, true);
    Document input;
    input.addImported("rounded", original);
    const auto stepPath = tmp.filePath("original.step");
    io::exportVisible(stepPath, input, io::ExchangeFormat::Step);

    Document doc;
    const auto id = doc.addImported("roundtrip", io::readStep(stepPath));
    const auto originalVolume = volume(doc.find(id)->shape);
    const auto moved = doc.addAdvanced(MoveFace{id, {{35, 25, 20}, {0, 0, 1}}, 5});
    check(almostEqual(height(doc.find(moved)->shape), 25), "planar face did not move five mm");
    const auto blends = blendFaces(doc.find(moved)->shape);
    check(blends.size() == 1, "expected one curved blend");
    const auto repaired = doc.addAdvanced(RemoveBlend{moved, blends.front()});
    check(almostEqual(volume(doc.find(repaired)->shape), 70*50*25), "adjacent planes were not restored");
    check(almostEqual(volume(doc.find(id)->shape), originalVolume), "direct edit damaged imported base");
    check(almostEqual(height(doc.find(moved)->shape), 25) && volume(doc.find(moved)->shape) > 0,
        "blend removal damaged its input");
    const auto project = tmp.filePath("direct.graph3d");
    io::saveProject(project, doc);
    const auto reopened = io::openProject(project);
    const auto finalStep = tmp.filePath("repaired.step");
    io::exportVisible(finalStep, reopened, io::ExchangeFormat::Step);
    const auto final = io::readStep(finalStep);
    check(almostEqual(height(final), 25) && almostEqual(volume(final), 70*50*25), "edited solid failed STEP roundtrip");
    const auto shorter = movePlanarFace(final, {{35, 25, 25}, {0, 0, 1}}, -3);
    check(almostEqual(height(shorter), 22) && almostEqual(volume(shorter), 70*50*22), "inward face displacement failed");
}

void tangentChain()
{
    Document doc;
    Sketch stadium;
    stadium.entities = {Line{{0, -10}, {30, -10}}, Arc{{30, -10}, {40, 0}, {30, 10}},
        Line{{30, 10}, {0, 10}}, Arc{{0, 10}, {-10, 0}, {0, -10}}};
    const auto body = doc.addExtrude(doc.addSketch(stadium), 12);
    const auto& shape = doc.find(body)->shape;
    const auto edges = twoFaceEdges(shape);
    const auto top = std::ranges::find_if(edges, [](const auto& e) {
        return std::ranges::all_of(e.samples, [](const auto& p) { return almostEqual(p[2], 12); });
    });
    check(top != edges.end(), "missing tangent rim");
    const auto sample = std::span(&*top, 1);
    bool rejected = false;
    try { treatEdges(shape, sample, EdgeTreatmentKind::Fillet, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "single edge selection changed a whole tangent chain");
    check(volume(treatEdges(shape, sample, EdgeTreatmentKind::Fillet, 1, true)) < volume(shape),
        "automatic tangent chain could not be treated");
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QTemporaryDir tmp;
    try {
        check(tmp.isValid(), "could not create temporary directory");
        if (argc > 1) {
            auto example = io::openProject(app.arguments().at(1));
            check(example.selectVariant("Large") && almostEqual(volume(example.find(2)->shape), 8*std::numbers::pi*(400-289)),
                "example family did not rebuild");
        }
        family(tmp);
        holesAndPatterns(tmp);
        directEdits(tmp);
        tangentChain();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
