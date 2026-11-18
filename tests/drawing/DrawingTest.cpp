#include "drawing/DrawingSheet.h"
#include "drawing/Specification.h"
#include "io/LinkedComponents.h"
#include "drawing/DraftCurves.h"
#include "io/DraftIO.h"
#include "io/DxfIO.h"
#include "io/FileIO.h"
#include "io/ModelViewsIO.h"
#include "geometry/SketchModeling.h"
#include "geometry/SketchConstraints.h"
#include "io/ProjectIO.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QLineF>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace draw = graph3d::drawing;
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

struct Example {
    const char* file;
    const char* title;
    const char* designation;
    double x, y, z, volume;
    std::vector<double> diameters;
};

const doc::Object& finishedPart(const doc::Document& model)
{
    const doc::Object* result = nullptr;
    for (const auto& object : model.objects()) {
        check(object.status == doc::Status::Ready, "Example failed to regenerate");
        if (object.visible) {
            check(!result && !std::holds_alternative<geom::Sketch>(object.parameters),
                  "An example needs exactly one visible result body");
            result = &object;
        }
    }
    check(result, "Example has no visible result");
    return *result;
}

void checkView(const draw::View& view, double width, double height)
{
    check(std::abs(view.width - width) < 1.e-6 && std::abs(view.height - height) < 1.e-6,
          "Incorrect orthographic dimensions");
    check(!view.visible.empty(), "Missing visible linework");
    double maxX = 0;
    double maxY = 0;
    for (const auto* group : {&view.visible, &view.hidden}) {
        for (const auto& line : *group) {
            check(line.size() >= 2, "An edge should contain at least two points");
            for (const auto& p : line) {
                check(std::isfinite(p.x) && std::isfinite(p.y), "Non-finite projection");
                check(std::abs(p.x) <= width / 2 + 0.03 && std::abs(p.y) <= height / 2 + 0.03,
                      "An edge fell outside a centered view");
                if (group == &view.visible) {
                    maxX = std::max(maxX, std::abs(p.x));
                    maxY = std::max(maxY, std::abs(p.y));
                }
            }
        }
    }
    check(maxX >= width / 2 - 0.03 && maxY >= height / 2 - 0.03,
          "Visible edges do not reach the expected drawing bounds");
}

void verifyExample(const QDir& source, const QDir& output, const Example& example)
{
    const auto name = QString::fromLatin1(example.file);
    const auto file = source.filePath(name + ".graph3d");
    auto model = io::openProject(file);
    const auto& body = finishedPart(model);
    GProp_GProps properties;
    BRepGProp::VolumeProperties(body.shape, properties);
    check(std::abs(properties.Mass() - example.volume) < example.volume * 1.e-7,
          "Unexpected solid volume in a demonstration project");

    const auto views = draw::project(body.shape);
    checkView(views.front, example.x, example.z);
    checkView(views.top, example.x, example.y);
    checkView(views.right, example.y, example.z);
    check(!views.front.hidden.empty(), "Expected hidden edges behind the front view");
    if (name == "flange") {
        check(views.top.visible.size() >= 6, "Flange top should show the outer rim and five holes");
    }

    std::vector<double> diameters;
    for (const auto& circle : views.top.circles) {
        diameters.push_back(circle.radius * 2);
    }
    std::sort(diameters.begin(), diameters.end());
    check(diameters.size() == example.diameters.size(), "A full circle was missed or counted twice");
    for (std::size_t i = 0; i < diameters.size(); ++i) {
        check(std::abs(diameters[i] - example.diameters[i]) < 1.e-5, "Incorrect analytic diameter");
    }

    const auto restoredFile = output.filePath(name + "-roundtrip.graph3d");
    io::saveProject(restoredFile, model);
    const auto reopened = io::openProject(restoredFile);
    const auto newViews = draw::project(finishedPart(reopened).shape);
    checkView(newViews.front, example.x, example.z);
    checkView(newViews.top, example.x, example.y);
    checkView(newViews.right, example.y, example.z);

    const auto pdf = output.filePath(name + ".pdf");
    draw::savePdf(pdf, views, {QString::fromLatin1(example.title), QString::fromLatin1(example.designation), true, true});
    QFile generated(pdf);
    check(generated.open(QIODevice::ReadOnly) && generated.size() > 1000
          && generated.read(5) == "%PDF-", "Drawing PDF was not written");

    std::array<double, 5> openTimes;
    std::array<double, 5> projectTimes;
    for (std::size_t i = 0; i < openTimes.size(); ++i) {
        QElapsedTimer clock;
        clock.start();
        const auto stage = io::openProject(file);
        openTimes[i] = clock.nsecsElapsed() / 1.e6;
        clock.restart();
        const auto projection = draw::project(finishedPart(stage).shape);
        projectTimes[i] = clock.nsecsElapsed() / 1.e6;
        check(!projection.front.visible.empty(), "Projection benchmark returned an empty result");
    }
    std::sort(openTimes.begin(), openTimes.end());
    std::sort(projectTimes.begin(), projectTimes.end());
    std::cout << example.file << ": open+rebuild=" << openTimes[2]
              << " ms, projection=" << projectTimes[2] << " ms (median of 5)\n";
}

double loopArea(const std::vector<draw::Point>& loop)
{
    double sum = 0;
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const auto& a = loop[i];
        const auto& b = loop[(i + 1) % loop.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return std::abs(sum) / 2;
}

void verifySections(const QDir& source, const QDir& output)
{
    const auto model = io::openProject(source.filePath("stepped-bushing.graph3d"));
    const auto& shape = finishedPart(model).shape;
    const auto views = draw::project(shape);
    for (const auto plane : {geom::Plane::XZ, geom::Plane::YZ}) {
        const auto axial = draw::section(shape, plane, 0);
        check(axial.material.size() == 2, "Axial section filled the bore or lost a wall");
        for (const auto& face : axial.material) {
            check(face.size() == 1 && std::abs(loopArea(face.front()) - 200) < 1.e-5,
                  "Axial section failed to show the stepped profile");
        }
        checkView(axial.view, 40, 30);
        if (plane == geom::Plane::XZ) {
            draw::savePdf(output.filePath("stepped-bushing-axial-section.pdf"), views,
                          {"Stepped bushing", "G3D-002"}, &axial);
        }
    }

    const auto transverse = draw::section(shape, geom::Plane::XY, 4);
    check(transverse.material.size() == 1 && transverse.material.front().size() == 2,
          "Transverse section did not retain the bore as an inner boundary");
    const auto& loops = transverse.material.front();
    check(std::abs(std::abs(loopArea(loops[0]) - loopArea(loops[1])) - 125 * std::numbers::pi) < 1.1,
          "The offset section was taken at the wrong height");

    QImage image(1188, 840, QImage::Format_RGB32);
    const draw::SheetOptions options{"Section test", "A-A", false, false};
    QPainter painter(&image);
    draw::paintSheet(painter, QRectF(image.rect()), views, options, &transverse);
    painter.end();
    // Top view is centered at (211, 126) mm. The bore is empty inside R10.
    for (int y = 499; y < 509; ++y) {
        for (int x = 839; x < 849; ++x) {
            check(image.pixelColor(x, y) == Qt::white, "Hatch escaped into the bore");
        }
    }
    int dark = 0;
    for (int y = 495; y < 513; ++y) {
        for (int x = 888; x < 897; ++x) {
            dark += image.pixelColor(x, y).lightness() < 190;
        }
    }
    check(dark > 5, "Cut material was not hatched");
    QFile pdf(output.filePath("stepped-bushing-section.pdf"));
    draw::savePdf(pdf.fileName(), views, options, &transverse);
    check(pdf.open(QIODevice::ReadOnly) && pdf.size() > 1000 && pdf.read(5) == "%PDF-",
          "Section PDF was not written");

    bool rejected = false;
    try {
        draw::section(shape, geom::Plane::XY, 15);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "A section on the body boundary was accepted");
}

void verifyMultiSheetDrawing(const QDir& source, const QDir& output)
{
    const auto modelPath = output.filePath("stepped-bushing-stage13-model.graph3d");
    io::saveProject(modelPath, io::openProject(source.filePath("stepped-bushing.graph3d")));
    const auto model = io::openModelSource(modelPath);
    const int body = finishedPart(model.document).id;

    draw::Draft draft;
    draft.title = QStringLiteral("Ступенчатая втулка");
    draft.number = QStringLiteral("G3D-002");
    draft.author = QStringLiteral("Graph3D");
    draft.a3 = true;
    draft.paper.eskd = true;
    draft.paper.material = QStringLiteral("Сталь 45");
    draft.paper.organization = QStringLiteral("Graph3D");
    draft.layers.push_back({QStringLiteral("Обозначения")});
    draft.layers.push_back({QStringLiteral("Вспомогательные"), false, true});
    draft.modelViews = io::buildModelViews(model, body, {}, draw::draftSize(draft));
    draft.modelViews->layout = {{{{188, 62}, 2}, {{70, 62}, 2}, {{188, 172}, 2}}};
    draft.modelViews->detail = draw::DetailView{0, {14.5, -4}, 7, {{310, 84}, 5}};
    draft.items = {
        {draw::DraftKind::Datum, {148, 80}, {}, {124, 98}, QStringLiteral("А")},
        {draw::DraftKind::Tolerance, {218, 54}, {}, {238, 46}, QStringLiteral("⊥ | 0,05 | А")},
        {draw::DraftKind::Roughness, {180, 32}, {}, {155, 23}, QStringLiteral("Ra 3,2")},
        {draw::DraftKind::Leader, {228, 85}, {}, {250, 118}, QStringLiteral("Бурт")},
        {draw::DraftKind::Text, {50, 190}, {}, {}, QStringLiteral("HIDDEN-LAYER-TEXT")}
    };
    for (auto& item : draft.items) {
        item.layer = 1;
    }
    draft.items.back().layer = 2;
    draw::Draft second;
    second.title = draft.title;
    second.number = draft.number;
    second.paper.eskd = true;
    second.paper.portrait = true;
    second.paper.requirements = QStringLiteral("Острые кромки притупить.\nУдалить заусенцы.");
    second.modelViews = io::buildModelViews(model, body, draw::SectionSettings{geom::Plane::XZ, 0}, draw::draftSize(second));
    second.modelViews->dimensions = false;
    second.modelViews->hiddenLines = false;
    second.modelViews->layout = {{{{147, 64}, 2}, {{66, 64}, 1}, {{147, 168}, 2}}};
    second.modelViews->local = draw::LocalSection{{13, 0}, 10};
    draft.additionalSheets.push_back(second);

    const auto path = output.filePath("stepped-bushing-two-sheets.g3draw");
    io::saveDraft(path, draft);
    const auto restored = io::openDraft(path);
    check(restored.additionalSheets.size() == 1 && restored.paper == draft.paper
        && restored.layers == draft.layers && restored.items.size() == draft.items.size(),
        "Drawing book, layers, or annotations did not round trip");
    const auto& page = restored.additionalSheets.front();
    check(draw::draftSize(restored) == QSizeF(420, 297) && draw::draftSize(page) == QSizeF(210, 297)
        && restored.modelViews->detail == draft.modelViews->detail && page.paper == second.paper
        && page.modelViews->local == second.modelViews->local && page.modelViews->geometry->section.has_value(),
        "Page orientation, detail, or local section did not round trip");
    check(restored.items[2].text == draft.items[2].text
        && !draw::layerVisible(restored, restored.items.back().layer)
        && !draw::layerEditable(restored, restored.items.back().layer),
        "Drawing layer flags or annotation text did not survive save and reopen");

    const auto render = [&](const draw::Draft& sheet) {
        QImage image(840, 1188, QImage::Format_RGB32);
        QPainter painter(&image);
        painter.scale(4, 4);
        draw::paintDraft(painter, sheet, false, -1, nullptr, 2, 2);
        painter.end();
        return image;
    };
    const auto local = render(page);
    auto original = page;
    auto uncut = std::make_shared<draw::ModelProjection>(*original.modelViews->geometry);
    uncut->section.reset();
    original.modelViews->geometry = std::move(uncut);
    original.modelViews->local.reset();
    const auto unsectioned = render(original);
    const QRect region(650, 235, 40, 40);
    check(local.copy(region) != unsectioned.copy(region), "The local section did not render in the selected region");
    const QRect outside(445, 192, 60, 80);
    check(local.copy(outside) == unsectioned.copy(outside), "The local section changed geometry outside its boundary");

    const auto pdfPath = output.filePath("stepped-bushing-two-sheets.pdf");
    draw::exportDraftPdf(pdfPath, restored);
    QFile pdf(pdfPath);
    check(pdf.open(QIODevice::ReadOnly), "Two-sheet drawing PDF did not open");
    const auto content = pdf.readAll();
    check(content.startsWith("%PDF-") && content.count("/Type /Page\n") == 2,
          "Drawing PDF did not contain two pages");
}

void verifyParametricBracket(const QDir& output)
{
    const auto path = output.filePath("parametric-bracket.graph3d");
    const auto drawingPath = output.filePath("parametric-bracket.g3draw");
    graph3d::document::Document initial;
    geom::Sketch sketch{geom::Plane::XY, {
        geom::Line{{0, 0}, {60, 0}}, geom::Line{{60, 0}, {60, 25}},
        geom::Line{{60, 25}, {28, 25}}, geom::Line{{28, 25}, {28, 45}},
        geom::Line{{28, 45}, {0, 45}}, geom::Line{{0, 45}, {0, 0}},
        geom::Circle{{14, 18}, 5}
    }};
    using enum geom::ConstraintKind;
    sketch.constraints = {{Length, {0, 0}, {}, 60}, {Diameter, {6, 0}, {}, 10},
                          {Length, {1, 0}, {}, 25}, {Length, {4, 0}, {}, 28},
                          {Length, {5, 0}, {}, 45}, {Perpendicular, {0, 0}, {1, 0}}};
    for (int i = 0; i < 6; ++i) {
        sketch.constraints.push_back({i % 2 == 0 ? Horizontal : Vertical, {i, 0}});
    }
    const auto sketchId = initial.addSketch(sketch);
    const auto body = initial.addExtrude(sketchId, 12);
    io::saveProject(path, initial);
    const auto firstSource = io::openModelSource(path);
    auto document = io::openProject(path);
    check(std::get<geom::Sketch>(document.find(sketchId)->parameters) == sketch,
          "The parametric sketch did not survive project save and reopen");
    GProp_GProps properties;
    BRepGProp::VolumeProperties(document.find(body)->shape, properties);
    const auto volume = (60 * 25 + 28 * 20 - 25 * std::numbers::pi) * 12;
    check(std::abs(properties.Mass() - volume) < .001, "The bracket's through cut was lost");

    draw::Draft draft;
    draft.a3 = true;
    draft.title = QStringLiteral("Parametric angle bracket");
    draft.number = QStringLiteral("G3D-014");
    draft.modelViews = io::buildModelViews(firstSource, body, {}, draw::draftSize(draft));
    const auto width = [](const draw::ModelViews& views) { return views.geometry->views.front.width; };
    check(std::abs(width(*draft.modelViews) - 60) < 1.e-5, "Initial linked drawing width is wrong");
    io::saveDraft(drawingPath, draft);

    auto edited = sketch;
    edited.constraints[0].value = 80;
    check(document.editSketch(sketchId, edited), "Driving width could not be edited");
    check(document.find(body)->status == graph3d::document::Status::Ready,
          "Dependent extrusion did not rebuild after the sketch edit");
    io::saveProject(path, document);
    auto restoredDrawing = io::openDraft(drawingPath);
    const auto secondSource = io::openModelSource(path);
    check(secondSource.digest != restoredDrawing.modelViews->sourceDigest, "The linked drawing was not marked outdated");
    auto updated = io::buildModelViews(secondSource, body, {}, draw::draftSize(draft), &*restoredDrawing.modelViews);
    check(std::abs(width(updated) - 80) < 1.e-5
        && std::abs(updated.geometry->views.top.height - 45) < 1.e-5
        && updated.layout == draft.modelViews->layout && updated.geometry->views.top.circles.size() == 1,
        "The rebuilt drawing lost its layout, hole, or new width");
    const auto updatedShape = document.find(body)->shape;
    check(document.undo() && std::get<geom::Sketch>(document.find(sketchId)->parameters) == sketch,
          "Undo did not restore the original driving sketch parameters");
    check(std::abs(draw::project(document.find(body)->shape).front.width - 60) < 1.e-5,
          "Undo did not restore the original body width");
    check(document.redo() && document.find(body)->shape.IsSame(updatedShape),
          "Redo did not restore the updated body");
    restoredDrawing.modelViews = std::move(updated);
    io::saveDraft(drawingPath, restoredDrawing);
    draw::exportDraftPdf(output.filePath("parametric-bracket.pdf"), io::openDraft(drawingPath));

    edited.constraints.push_back({Length, {0, 0}, {}, 100});
    const auto revision = document.revision();
    bool rejected = false;
    try {
        document.editSketch(sketchId, edited);
    } catch (const std::invalid_argument& error) {
        rejected = std::string(error.what()).find("conflict") != std::string::npos;
    }
    check(rejected && document.revision() == revision && document.find(body)->shape.IsSame(updatedShape),
          "An invalid sketch edit modified the committed model");
}

void verifySketchOnFace(const QDir& output)
{
    using graph3d::document::Status;
    graph3d::document::Document document;
    const auto base = document.addSketch({geom::Plane::XY, {geom::Rectangle{{0, 0}, {50, 40}}}});
    const auto support = document.addExtrude(base, 10);
    geom::Sketch sketch{geom::Plane::XY, {geom::Rectangle{{50, 0}, {65, 12}}}};
    sketch.support = geom::SketchSupport{support, {{15, 15, 10}, {0, 0, 1}}};
    const auto context = document.sketchContext(sketch);
    const auto edge = std::ranges::find_if(context.projections, [](const auto& projection) {
        return projection.points.front().v == 0 && projection.points.back().v == 0;
    });
    check(edge != context.projections.end(), "The bottom edge was not projected from the selected face");
    const double position = edge->points.front().u > edge->points.back().u ? 0 : 1;
    sketch.constraints.push_back({geom::ConstraintKind::OnProjection, {0, 0}, {}, position, edge->edge});
    const auto attached = document.addSketch(sketch);
    check(document.find(support)->visible, "Adding an attached sketch hid the base body");
    const auto result = document.addExtrude(attached, 5);
    check(document.editExtrude(support, 18), "Support height edit failed");
    auto current = document.sketchContext(sketch);
    check(std::abs(current.frame.Location().Z() - 18) < 1.e-6 && document.find(result)->status == Status::Ready,
          "An attached sketch failed to follow its moved support plane");
    GProp_GProps properties;
    BRepGProp::VolumeProperties(document.find(result)->shape, properties);
    check(std::abs(properties.CentreOfMass().Z() - 20.5) < 1.e-6,
          "Attached extrusion used the wrong support orientation");
    check(document.editSketch(base, {geom::Plane::XY, {geom::Rectangle{{0, 0}, {60, 40}}}}),
          "Support width edit failed");
    current = document.sketchContext(sketch);
    const auto solved = geom::solveSketch(sketch, current.projections);
    check(solved.conflicts.empty() && std::abs(std::get<geom::Rectangle>(solved.entities[0]).first.u - 60) < 1.e-6,
          "A projected endpoint failed to follow the remodeled edge");
    const auto path = output.filePath("face-sketch.graph3d");
    io::saveProject(path, document);
    auto restored = io::openProject(path);
    check(std::get<geom::Sketch>(restored.find(attached)->parameters) == sketch && restored.find(result)->status == Status::Ready,
          "Face placement or projected edge references did not round trip");
    check(restored.editSketch(base, {geom::Plane::XY, {geom::Circle{{25, 20}, 24}}}),
          "Replacing the support outline failed");
    check(restored.find(attached)->status == Status::Failed && restored.find(result)->status == Status::Blocked
        && restored.find(attached)->error.find("projected sketch edge") != std::string::npos,
        "A lost projected edge did not produce a diagnostic and block the dependent solid");
    const auto brokenPath = output.filePath("face-sketch-lost-projection.graph3d");
    io::saveProject(brokenPath, restored);
    check(io::openProject(brokenPath).find(attached)->status == Status::Failed,
        "A lost projected edge prevented reopening the model");
    check(restored.undo() && restored.find(result)->status == Status::Ready,
        "Undo did not restore the lost projection");
}

void verifyAssemblyDrawing(const QDir& output)
{
    check(output.mkpath("assembly-parts"), "Could not create the assembly source directory");
    const auto platePath = output.filePath("assembly-parts/base-plate.graph3d");
    const auto studPath = output.filePath("assembly-parts/stud.graph3d");
    const auto modelPath = output.filePath("fixture-assembly.graph3d");
    const auto draftPath = output.filePath("fixture-assembly.g3draw");
    doc::Document plate, stud, assembly;
    const auto baseSketch = plate.addSketch({geom::Plane::XY, {geom::Rectangle{{-60, -35}, {60, 35}}}});
    const auto base = plate.addExtrude(baseSketch, 10);
    plate.setPartProperties(base, {"G3D.017.01", "Base plate", "Steel", "120 × 70 mm"});
    const auto studSketch = stud.addSketch({geom::Plane::XY, {geom::Circle{{0, 0}, 8}}});
    const auto stem = stud.addExtrude(studSketch, 18);
    stud.setPartProperties(stem, {"G3D.017.02", "Locating stud", "Steel", "Ø16 mm"});
    io::saveProject(platePath, plate);
    io::saveProject(studPath, stud);
    const auto fixed = assembly.addLinkedComponent(io::readLinkedComponent(platePath, base, modelPath));
    const auto first = assembly.addLinkedComponent(io::readLinkedComponent(studPath, stem, modelPath));
    const auto second = assembly.addLinkedComponent(io::readLinkedComponent(studPath, stem, modelPath));
    doc::Assembly parts{{{fixed, {}, true}, {first, {0, {-33, 0, 10}, {}}}}};
    const auto id = assembly.addAssembly(parts);
    assembly.setVisible(second, false);
    assembly.setPartProperties(id, {"G3D.017.00", "Locating fixture", {}, {}});
    io::saveProject(modelPath, assembly);
    auto source = io::openModelSource(modelPath);
    draw::Draft draft;
    draft.title = QStringLiteral("Locating fixture");
    draft.number = QStringLiteral("G3D.017.00");
    draft.modelViews = io::buildModelViews(source, id, {}, draw::draftSize(draft));
    draft.modelViews->dimensions = false;
    draft.modelViews->hiddenLines = false;
    draft.modelViews->layout = {{{{167, 49}, 1}, {{64, 49}, 1}, {{167, 121}, 1}}};
    auto specification = std::make_shared<draw::AssemblyDrawing>(*draft.modelViews->assembly);
    check(specification->rows.size() == 2 && specification->rows[1].instances.size() == 1,
          "Initial specification rows were not generated");
    check(specification->rows[1].designation == "G3D.017.02" && specification->rows[1].material == "Steel",
          "Specification did not use source metadata");
    specification->callouts = {{1, 0, 2, {50, 20}, {238, 153}}, {2, 0, 2, {3, 4}, {111, 172}}};
    draft.modelViews->assembly = specification;
    io::saveDraft(draftPath, draft);
    auto saved = io::openDraft(draftPath);
    check(*saved.modelViews->assembly == *specification, "Specification or positions were not persisted");

    parts.components.push_back({second, {0, {33, 0, 10}, {}}});
    check(assembly.editAssembly(id, parts), "Editing an existing assembly rejected a new repeated component");
    io::saveProject(modelPath, assembly);
    source = io::openModelSource(modelPath);
    check(saved.modelViews->sourceDigest != source.digest, "Assembly edit did not make the views stale");
    auto updated = io::buildModelViews(source, id, {}, draw::draftSize(saved), &*saved.modelViews);
    const auto& rows = updated.assembly->rows;
    check(rows.size() == 2 && rows[1].position == 2 && rows[1].instances.size() == 2,
          "A duplicate source did not increase quantity in the existing row");
    check(updated.layout == saved.modelViews->layout && updated.assembly->callouts == specification->callouts
        && std::abs(draw::positionOrigin(updated, rows[1], 1, 2).x() - 200) < 1.e-6, "Position associations or the updated pose were lost");
    auto labels = std::make_shared<draw::AssemblyDrawing>(*updated.assembly);
    labels->callouts.push_back({2, 1, 2, {-4, -2}, {222, 83}});
    updated.assembly = std::move(labels);
    saved.modelViews = updated;
    io::saveDraft(draftPath, saved);
    saved = io::openDraft(draftPath);
    check(saved.modelViews->assembly->callouts.size() == 3 && saved.modelViews->assembly->rows[1].instances.size() == 2,
          "Updated specification or a repeated position was lost in the saved file");

    const auto pdfPath = output.filePath("fixture-assembly.pdf");
    draw::exportDraftPdf(pdfPath, saved);
    const auto dxf = output.filePath("fixture-assembly.dxf");
    check(io::exportDxf(dxf, saved) > 0, "DXF did not report omitted assembly callouts");
    const auto exported = io::importDxf(dxf);
    check(!exported.drawing.items.empty() && std::ranges::all_of(exported.drawing.items, [](const auto& item) {
        return item.kind == draw::DraftKind::Polyline;
    }), "Linked assembly lost projected linework in DXF");
    QFile pdf(pdfPath);
    check(pdf.open(QIODevice::ReadOnly) && pdf.readAll().count("/Type /Page\n") == 2,
          "Export did not contain the drawing and its specification");
    auto longTable = *saved.modelViews->assembly;
    longTable.rows.clear();
    for (int i = 1; i <= 100; ++i) {
        auto row = saved.modelViews->assembly->rows.front();
        row.position = i;
        row.note = QString(1000, u'Ш');
        longTable.rows.push_back(std::move(row));
    }
    const auto pages = draw::paginateSpecification(longTable);
    qsizetype printed = 0;
    for (const auto& page : pages) {
        for (const auto& band : page) {
            for (const auto& line : band.cells.back()) {
                printed += line.size();
            }
        }
    }
    check(pages.size() > 2 && printed == 100000, "Pagination lost long notes");

    auto reduced = parts;
    reduced.components = {{fixed, {}, true}, {fixed, {0, {0, 0, 10}, {}}}};
    check(assembly.editAssembly(id, reduced), "Could not remove an existing part position");
    io::saveProject(modelPath, assembly);
    const auto afterRemoval = io::buildModelViews(io::openModelSource(modelPath), id, {}, draw::draftSize(saved), &updated);
    check(afterRemoval.assembly->rows.size() == 1 && afterRemoval.assembly->callouts.size() == 1
        && afterRemoval.assembly->callouts.front().position == 1,
        "Removing a position left obsolete callouts");
    check(assembly.undo(), "Assembly composition change was not undoable");
    io::saveProject(modelPath, assembly);
}

void verifyDraft(const QDir& output)
{
    using enum draw::DraftKind;
    const draw::Draft draft{"Mounting plate", "G3D-2D-001", "Graph3D", false, {
        {Rectangle, {70, 55}, {210, 145}},
        {Circle, {105, 100}, {117, 100}},
        {Circle, {175, 100}, {187, 100}},
        {Line, {85, 100}, {196, 100}, {}, {}, draw::DraftStroke::Center},
        {Horizontal, {70, 55}, {210, 55}, {140, 38}},
        {Vertical, {70, 55}, {70, 145}, {48, 100}},
        {Text, {70, 161}, {}, {}, "2 x holes"},
    }};
    const auto path = output.filePath("mounting-plate.g3draw");
    io::saveDraft(path, draft);
    auto reopened = io::openDraft(path);
    for (auto& item : reopened.items) {
        check(!item.id.isNull() && !item.anchorA && !item.anchorB, "Legacy drawing gained an attachment");
        item.id = QUuid{};
    }
    check(reopened.items == draft.items && reopened.title == draft.title && reopened.number == draft.number
          && reopened.author == draft.author && reopened.a3 == draft.a3, "Draft data changed after reloading");
    check(draw::draftHit(draft.items[1], {117, 100}, .1)
          && !draw::draftHit(draft.items[1], {105, 100}, .1), "Circle selection ignores the circumference");
    QFile pdf(output.filePath("mounting-plate.pdf"));
    draw::exportDraftPdf(pdf.fileName(), reopened);
    check(pdf.open(QIODevice::ReadOnly) && pdf.read(5) == "%PDF-" && pdf.size() > 1000,
          "2D draft PDF was not produced");
    QFile bad(output.filePath("invalid.g3draw"));
    check(bad.open(QIODevice::WriteOnly) && bad.write("{}") == 2, "Could not write invalid drawing");
    bad.close();
    bool rejected = false;
    try {
        io::openDraft(bad.fileName());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "An invalid native file was accepted");
}

void verifyAttachedDimensions(const QDir& output)
{
    using enum draw::DraftKind;
    using enum draw::DraftAnchor;
    draw::Draft draft;
    draft.items = {{Line, {40, 40}, {90, 40}}, {Rectangle, {100, 55}, {140, 85}},
                   {Circle, {160, 90}, {170, 90}}, {Horizontal, {40, 40}, {90, 40}, {65, 25}},
                   {Vertical, {100, 85}, {160, 100}, {185, 95}},
                   {Horizontal, {45, 70}, {75, 70}, {60, 60}}};
    for (auto& item : draft.items) {
        item.id = QUuid::createUuid();
    }
    draft.items[3].anchorA = draw::DraftReference{draft.items[0].id, Start};
    draft.items[3].anchorB = draw::DraftReference{draft.items[0].id, End};
    draft.items[4].anchorA = draw::DraftReference{draft.items[1].id, CornerAB};
    draft.items[4].anchorB = draw::DraftReference{draft.items[2].id, Bottom};

    draft.items[0].a = {45, 55};
    draft.items[0].b = {115, 55};
    draft.items[1].b = {145, 90};
    draft.items[2].b = {175, 90};
    const draw::DraftGeometry geometry(draft);
    const auto first = geometry.resolve(draft.items[3]);
    check(first.a == QPointF(45, 55) && first.b == QPointF(115, 55) && first.label.y() == 40,
          "Line edit did not update its attached dimension and offset");
    const auto second = geometry.resolve(draft.items[4]);
    check(second.a == QPointF(100, 90) && second.b == QPointF(160, 105),
          "Rectangle corner or circle quadrant did not update");
    check(geometry.resolve(draft.items[5]) == draft.items[5], "A free dimension moved");
    auto preview = draft.items[0];
    preview.b = {145, 55};
    check(draw::DraftGeometry(draft, &preview).resolve(draft.items[3]).b == preview.b,
          "Dimension did not follow the drag preview");

    const auto path = output.filePath("attached-dimensions.g3draw");
    io::saveDraft(path, draft);
    const auto restored = io::openDraft(path);
    check(restored.items == draft.items, "Dimension IDs or attachments did not survive saving");
    check(draw::DraftGeometry(restored).resolve(restored.items[3]) == first,
          "Reloaded attachment resolved to a different point");
    QFile pdf(output.filePath("attached-dimensions.pdf"));
    draw::exportDraftPdf(pdf.fileName(), restored);
    check(pdf.open(QIODevice::ReadOnly) && pdf.read(5) == "%PDF-", "PDF with attached dimensions was not written");

    draft.items[3].anchorA->item = QUuid::createUuid();
    bool rejected = false;
    try {
        io::saveDraft(path, draft);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "A dangling attachment was saved");
}

void verifyModelViews(const QDir& output)
{
    auto project = doc::Document{};
    const auto profile = project.addSketch({geom::Plane::XY, {geom::Circle{{0, 0}, 40}}});
    const auto plate = project.addExtrude(profile, 15);
    const auto hole = project.addHole(plate, {{0, 0, 15}, {0, 0, 1}}, {10, 15, true});
    QTemporaryDir source;
    const QString original = source.filePath("flange.graph3d");
    const QString moved = source.filePath("flange-moved.graph3d");
    io::saveProject(original, project);
    const auto cut = draw::SectionSettings{geom::Plane::XZ, 0};
    draw::Draft draft{"Linked flange", "G3D-011", "Graph3D"};
    draft.items.push_back({draw::DraftKind::Text, {34, 164}, {}, {}, "Machine edges"});
    draft.modelViews = io::buildModelViews(io::openModelSource(original), hole, cut, draw::draftSize(draft));
    draft.modelViews->layout[0] = {{195, 60}, 0.75};
    draft.modelViews->layout[2] = {{213, 126}, 1};
    const auto layout = draft.modelViews->layout;
    const auto draftPath = output.filePath("flange-linked.g3draw");
    io::saveDraft(draftPath, draft);
    auto restored = io::openDraft(draftPath);

    const auto hasRadius = [](const draw::ModelViews& model, double radius) {
        return std::ranges::any_of(model.geometry->views.top.circles,
            [radius](const auto& circle) { return std::abs(circle.radius - radius) < 0.001; });
    };
    check(restored.items.size() == 1 && restored.items.front().text == "Machine edges", "Manual note was lost");
    check(restored.modelViews && restored.modelViews->bodyId == hole && restored.modelViews->layout == layout
          && draw::sectionSettings(*restored.modelViews) == cut && hasRadius(*restored.modelViews, 5),
          "Model view layout, hole, or cut did not survive saving");
    check(restored.modelViews->sourceDigest == io::currentModelDigest(original), "Saved digest is inconsistent");
    check(project.editHole(hole, {12, 15, true}), "Could not change the hole diameter");
    io::saveProject(original, project);
    const auto sourceV2 = io::openModelSource(original);
    check(sourceV2.digest != restored.modelViews->sourceDigest, "Edit did not invalidate the saved views");
    auto updated = io::buildModelViews(sourceV2, hole, cut, draw::draftSize(draft), &*restored.modelViews);
    check(updated.layout == layout && draw::sectionSettings(updated) == cut && !hasRadius(updated, 5)
          && hasRadius(updated, 6), "Refresh did not update the hole or preserve placement");
    const auto& walls = updated.geometry->section->material;
    check(walls.size() == 2, "Section failed to show the two walls of the hole");
    double nearest = 40;
    for (const auto& face : walls) {
        for (const auto& loop : face) {
            for (const auto& point : loop) {
                nearest = std::min(nearest, std::abs(point.x));
            }
        }
    }
    check(std::abs(nearest - 6) < .001, "Section radius did not update");

    restored.modelViews = updated;
    io::saveDraft(draftPath, restored);
    check(QFile::rename(original, moved), "Could not move the source model");
    auto cached = io::openDraft(draftPath);
    check(cached.modelViews && hasRadius(*cached.modelViews, 6) && cached.items.front().text == "Machine edges",
          "Cached geometry or manual note was lost with the model");
    cached.modelViews = io::buildModelViews(io::openModelSource(moved), hole, cut, draw::draftSize(cached),
                                            &*cached.modelViews);
    check(cached.modelViews->sourcePath == moved && cached.modelViews->layout == layout,
          "Relinking changed the view layout or used the wrong source");
    const auto demoModel = output.filePath("flange-linked.graph3d");
    io::saveProject(demoModel, project);
    cached.modelViews = io::buildModelViews(io::openModelSource(demoModel), hole, cut, draw::draftSize(cached),
                                            &*cached.modelViews);
    io::saveDraft(draftPath, cached);
    QFile native(draftPath);
    check(native.open(QIODevice::ReadOnly), "Could not read saved drawing");
    const auto root = QJsonDocument::fromJson(native.readAll()).object();
    check(root.value("version").toInt() == 3 && root.value("modelViews").toObject().value("source") == "flange-linked.graph3d",
          "Linked drawing did not store a relative file location");
    QFile pdf(output.filePath("flange-linked.pdf"));
    draw::exportDraftPdf(pdf.fileName(), cached);
    check(pdf.open(QIODevice::ReadOnly) && pdf.size() > 1500 && pdf.read(5) == "%PDF-",
          "Could not export the combined drawing");
}

void verifyDraftTools(const QDir& output)
{
    using enum draw::DraftKind;
    draw::Draft draft{"Plate", "G3D-012", "Graph3D"};
    const auto add = [&](draw::DraftItem item) {
        item.id = QUuid::createUuid();
        draft.items.push_back(std::move(item));
    };
    add({Arc, {60, 45}, {40, 77.5}, {60, 110}});
    draw::DraftItem profile{Polyline};
    profile.points = {{200, 45}, {200, 110}, {60, 110}};
    add(profile);
    add({Line, {35, 45}, {200, 45}});
    add({Circle, {90, 70}, {97, 70}});
    add({Circle, {150, 85}, {159, 85}});

    const auto trimmed = draw::trimDraft(draft.items[2], draft, {44, 45});
    check(trimmed.size() == 1 && std::abs(trimmed[0].a.x() - 60) < .001
          && trimmed[0].b == QPointF(200, 45), "Trimming failed to stop at the arc");
    draft.items[2] = trimmed[0];

    const auto hatch = draw::hatchDraft(draft, {75, 65});
    check(hatch && hatch->loops.size() == 3, "Hatch failed to include both inner holes");
    check(draw::draftHit(*hatch, {75, 65}, .1) && !draw::draftHit(*hatch, {90, 70}, .1)
          && !draw::draftHit(*hatch, {150, 85}, .1), "A hole was filled by the hatch");
    add(*hatch);

    add({Line, {211, 140}, {225, 140}});
    add({Line, {246, 120}, {246, 154}});
    const auto extension = draw::extendDraft(draft.items[6], draft, {225, 140});
    check(extension && extension->b == QPointF(246, 140), "Extending failed to meet the boundary");
    draft.items[6] = *extension;
    const auto parallel = draw::offsetDraft(draft.items[6], 5);
    check(parallel && parallel->a.y() == 135 && parallel->b.y() == 135, "Offset distance is incorrect");
    add(*parallel);
    const auto copiedArc = draw::offsetDraft(draft.items[0], 4);
    check(copiedArc && std::abs(draw::draftCurves(*copiedArc).front().radius
                              - draw::draftCurves(draft.items[0]).front().radius - 4) < .001,
          "Offset arc failed");

    draw::DraftItem diameter{Diameter, {90, 70}, {97, 70}, {116, 61}};
    diameter.anchorA = draw::DraftReference{draft.items[3].id, draw::DraftAnchor::Center};
    diameter.anchorB = draw::DraftReference{draft.items[3].id, draw::DraftAnchor::Curve, 0};
    add(diameter);
    auto radius = diameter;
    radius.kind = Radius;
    radius.label = {95, 91};
    add(radius);
    draw::DraftItem angular{Angular, {200, 45}, {60, 45}, {185, 60}};
    angular.anchorA = draw::DraftReference{draft.items[2].id, draw::DraftAnchor::End};
    angular.anchorB = draw::DraftReference{draft.items[2].id, draw::DraftAnchor::Start};
    angular.anchorC = draw::DraftReference{draft.items[1].id, draw::DraftAnchor::Curve, 1};
    angular.points = {{200, 110}};
    add(angular);

    draw::DraftItem aligned{Aligned, {130, 45}, {150, 85}, {123, 70}};
    aligned.anchorA = draw::DraftReference{draft.items[2].id, draw::DraftAnchor::Curve, .5};
    aligned.anchorB = draw::DraftReference{draft.items[4].id, draw::DraftAnchor::Center};
    add(aligned);
    draw::DraftItem crossing{Horizontal, {246, 140}, {211, 140}, {225, 164}};
    crossing.anchorA = draw::DraftReference{draft.items[6].id, draw::DraftAnchor::Intersection, 1, draft.items[7].id};
    crossing.anchorB = draw::DraftReference{draft.items[6].id, draw::DraftAnchor::Start};
    add(crossing);

    draft.items[3].b = {100, 70};
    const draw::DraftGeometry resolved(draft);
    check(draw::draftDimensionText(resolved.resolve(draft.items[9])) == "⌀20", "Diameter did not update");
    check(draw::draftDimensionText(resolved.resolve(draft.items[10])) == "R10", "Radius did not update");
    check(draw::draftDimensionText(resolved.resolve(draft.items[11])) == "90°", "Wrong angle");
    check(draw::draftDimensionText(resolved.resolve(draft.items[12])) == "44.72136", "Wrong aligned length");
    check(draw::draftDimensionText(resolved.resolve(draft.items[13])) == "35", "Wrong intersection attachment");

    // Recreate the fill after the boundary edit, then verify the saved vector sheet.
    draft.items[5].loops = draw::hatchDraft(draft, {75, 65})->loops;
    const auto path = output.filePath("rounded-plate.g3draw");
    io::saveDraft(path, draft);
    const auto reopened = io::openDraft(path);
    check(reopened.items == draft.items, "Stage 12 geometry or references did not round-trip");
    const auto text = draw::draftDimensionText(draw::DraftGeometry(reopened).resolve(reopened.items[9]));
    check(text == "⌀20", "Reopened drawing lost the diameter association");
    QFile pdf(output.filePath("rounded-plate.pdf"));
    draw::exportDraftPdf(pdf.fileName(), reopened);
    check(pdf.open(QIODevice::ReadOnly) && pdf.size() > 1000 && pdf.read(5) == "%PDF-", "PDF export failed");
}

void verifyLinkedDimensions(const QDir& output)
{
    using enum draw::DraftKind;
    doc::Document model;
    geom::Sketch profile{geom::Plane::XY, {geom::Rectangle{{0, 0}, {100, 60}}, geom::Circle{{30, 20}, 5}}};
    const auto sketch = model.addSketch(profile);
    const auto body = model.addExtrude(sketch, 15);
    model.setPartProperties(body, {"G3D-019", "Test plate", "Steel", ""});
    model.setDensity(body, 7850);
    const auto source = output.filePath("stage19-plate.graph3d");
    const auto file = output.filePath("stage19-plate.g3draw");
    io::saveProject(source, model);

    draw::Draft sheet;
    sheet.paper.modelTitle = true;
    sheet.modelViews = io::buildModelViews(io::openModelSource(source), body, {}, draw::draftSize(sheet));
    const auto center = QPointF(180, 120);
    sheet.modelViews->layout[2] = {center, .5};
    const auto bore = draw::pickModelGeometry(*sheet.modelViews, center + QPointF(-7.5, 5), .1, true);
    const auto left = draw::pickModelGeometry(*sheet.modelViews, center + QPointF(-25, -4), .1);
    const auto right = draw::pickModelGeometry(*sheet.modelViews, center + QPointF(25, -4), .1);
    check(bore && left && right, "Could not pick a hole or outer edge on the linked view");
    auto axis = bore->reference;
    axis.anchor = draw::DraftAnchor::ModelCenter;
    axis.parameter = 0;
    const auto radialCenter = draw::DraftGeometry(sheet).point(axis);
    check(radialCenter.has_value(), "Could not resolve the centre of the hole");
    draw::DraftItem diameter{Diameter, *radialCenter, bore->position, {225, 135}};
    diameter.id = QUuid::createUuid();
    diameter.anchorA = axis;
    diameter.anchorB = bore->reference;
    draw::DraftItem width{Horizontal, left->position, right->position, {180, 153}};
    width.id = QUuid::createUuid();
    width.anchorA = left->reference;
    width.anchorB = right->reference;
    sheet.items = {diameter, width};
    auto incomplete = width;
    incomplete.anchorB.reset();
    check(draw::DraftGeometry(sheet).resolve(incomplete).lostReference, "Half a model attachment has no model scale");
    const auto verify = [](const draw::Draft& drawing, const QString& d, const QString& w) {
        const draw::DraftGeometry resolver(drawing);
        const auto measured = resolver.resolve(drawing.items[0]);
        const auto measuredWidth = resolver.resolve(drawing.items[1]);
        check(draw::draftDimensionText(measured) == d && draw::draftDimensionText(measuredWidth) == w,
              "Linked dimensions lost size, reference or model scale");
        check(measured.label == drawing.items[0].label && measuredWidth.label == drawing.items[1].label,
              "Rebuilding changed the manually placed label");
    };
    verify(sheet, QStringLiteral("⌀10"), QStringLiteral("100"));
    io::saveDraft(file, sheet);
    sheet = io::openDraft(file);
    verify(sheet, QStringLiteral("⌀10"), QStringLiteral("100"));

    std::get<geom::Circle>(profile.entities[1]).radius = 6;
    std::get<geom::Rectangle>(profile.entities[0]).opposite.u = 120;
    check(model.editSketch(sketch, profile), "Could not change the size of the plate and hole");
    io::saveProject(source, model);
    sheet.modelViews = io::buildModelViews(io::openModelSource(source), body, {}, draw::draftSize(sheet), &*sheet.modelViews);
    verify(sheet, QStringLiteral("⌀12"), QStringLiteral("120"));
    io::saveDraft(file, sheet);
    sheet = io::openDraft(file);
    verify(sheet, QStringLiteral("⌀12"), QStringLiteral("120"));
    check(draw::draftTitleField(sheet, draw::TitleField::Title, 1, 1) == QStringLiteral("Test plate")
        && draw::draftTitleField(sheet, draw::TitleField::Designation, 1, 1) == QStringLiteral("G3D-019")
        && draw::draftTitleField(sheet, draw::TitleField::Mass, 1, 1).toDouble() > 0,
        "The linked title block did not preserve part properties and mass");
    draw::exportDraftPdf(output.filePath("stage19-dimensions.pdf"), sheet);

    profile.entities.pop_back();
    check(model.editSketch(sketch, profile), "Could not remove the circular profile");
    io::saveProject(source, model);
    sheet.modelViews = io::buildModelViews(io::openModelSource(source), body, {}, draw::draftSize(sheet), &*sheet.modelViews);
    check(draw::DraftGeometry(sheet).resolve(sheet.items[0]).lostReference, "Deleted geometry must break the diameter link");
    io::saveDraft(file, sheet);
    const auto orphaned = io::openDraft(file);
    check(draw::DraftGeometry(orphaned).resolve(orphaned.items[0]).lostReference,
        "A lost association must survive save and open");
    draw::exportDraftPdf(output.filePath("stage19-broken-link.pdf"), orphaned);
}

void verifyDrawingViews(const QDir& output)
{
    using enum draw::DrawingViewKind;
    doc::Document model;
    const auto sketch = model.addSketch({geom::Plane::XY, {
        geom::Rectangle{{-50, -30}, {50, 30}}, geom::Circle{{-25, -10}, 5}, geom::Circle{{24, 10}, 5}}});
    const auto body = model.addExtrude(sketch, 20);
    const auto source = output.filePath("stage20-case.graph3d");
    const auto file = output.filePath("stage20-views.g3draw");
    io::saveProject(source, model);

    draw::Draft drawing;
    drawing.a3 = true;
    auto settings = io::buildModelViews(io::openModelSource(source), body, {}, draw::draftSize(drawing));
    const auto top = settings.geometry->views.top.id;
    const auto add = [&](draw::DrawingViewKind kind, const char* name, QPointF center, std::vector<QPointF> path = {}) {
        draw::DrawingView view;
        view.id = QUuid::createUuid();
        view.name = QString::fromLatin1(name);
        view.kind = kind;
        view.source = draw::derivedView(kind) ? top : QUuid{};
        view.placement = {center, kind == Detail ? 1.5 : .5};
        view.path = std::move(path);
        settings.additional.push_back(std::move(view));
    };
    add(Isometric, "ISO", {235, 60});
    add(Arrow, "K", {345, 60}, {{30, 20}, {44, 12}});
    add(SteppedSection, "A-A", {105, 145}, {{-62, -10}, {0, -10}, {0, 10}, {62, 10}});
    add(AlignedSection, "B-B", {222, 145}, {{-83, -16}, {0, -10}, {65, 14}});
    add(Detail, "C", {315, 139});
    settings.additional.back().focus = {-25, -10};
    add(Detail, "D", {365, 139});
    settings.additional.back().focus = {24, 10};
    add(Local, "E", {});
    settings.additional.back().focus = {-25, -10};
    add(Local, "F", {});
    settings.additional.back().focus = {24, 10};
    settings.showBase = {false, false, true};
    settings.dimensions = false;
    settings.layout[2] = {{106, 63}, .75};
    drawing.modelViews = io::buildModelViews(io::openModelSource(source), body, {}, draw::draftSize(drawing), &settings);
    const auto& ready = drawing.modelViews->additional;
    check(ready.size() == 8 && ready[2].geometry->material.size() >= 3 && !ready[3].geometry->material.empty(),
          "Complex sections did not cut through the model");
    check(ready[4].geometry->view.id == ready[4].id && ready[5].geometry->view.id != ready[4].id,
          "Details must have independent IDs");
    io::saveDraft(file, drawing);
    drawing = io::openDraft(file);
    check(!drawing.modelViews->showBase[0] && drawing.modelViews->additional[2].placement == settings.additional[2].placement,
          "Drawing view placement or visibility did not survive save");
    check(model.editExtrude(body, 25), "Could not change the case height");
    io::saveProject(source, model);
    drawing.modelViews = io::buildModelViews(io::openModelSource(source), body, {}, draw::draftSize(drawing), &*drawing.modelViews);
    const auto& refreshed = drawing.modelViews->additional;
    check(refreshed[2].geometry->view.height == 25 && refreshed[2].source == top
        && refreshed[3].id == settings.additional[3].id && refreshed[3].path == settings.additional[3].path,
        "Model update did not preserve derived view dependencies");
    draw::exportDraftPdf(output.filePath("stage20-views.pdf"), drawing);
}

void verifyExtendedDraft(const QDir& output)
{
    using enum draw::DraftKind;
    draw::Draft drawing;
    drawing.title = QStringLiteral("Template drawing");
    drawing.paper.titleBlock = draw::TitleBlockLayout{155, 6, {
        {QStringLiteral("Name"), draw::TitleField::Title, {}},
        {QStringLiteral("Scale"), draw::TitleField::Text, QStringLiteral("1:1")}}};
    drawing.additionalSheets.emplace_back();
    auto& second = drawing.additionalSheets.back();
    second.title = QStringLiteral("Detail and notes");
    second.paper.titleBlock = drawing.paper.titleBlock;
    second.items.push_back({Ellipse, {80, 55}, {145, 100}});
    second.items.push_back({Text, {70, 119}, {}, {}, QStringLiteral("Detail of the open contour")});
    const auto add = [&](draw::DraftItem item) {
        item.id = QUuid::createUuid();
        drawing.items.push_back(std::move(item));
    };
    add({Rectangle, {35, 35}, {135, 105}});
    add({Ellipse, {69, 57}, {101, 79}});
    draw::DraftItem spline{Spline};
    spline.points = {{160, 45}, {175, 67}, {215, 55}, {237, 73}};
    add(spline);
    add({ConstructionLine, {45, 130}, {125, 138}});

    const auto hatch = draw::hatchDraft(drawing, {50, 55});
    check(hatch && hatch->hatchBoundaries.size() == 2, "Hatch did not attach the ellipse hole and outline");
    add(*hatch);
    const auto oldHatch = drawing.items.back();
    drawing.items[0].b = {145, 115};
    const auto newFill = draw::DraftGeometry(drawing).resolve(oldHatch);
    check(!newFill.lostReference && newFill.loops != oldHatch.loops && newFill.loops.size() == 2,
          "Hatch did not follow its changed boundary");
    const auto outer = drawing.items[0];
    drawing.items.erase(drawing.items.begin());
    check(draw::DraftGeometry(drawing).resolve(oldHatch).lostReference,
          "Deleting the boundary did not invalidate the hatch");
    drawing.items.insert(drawing.items.begin(), outer);

    draw::DraftItem piece{Ellipse, {1, 1}, {12, 7}};
    piece.id = QUuid::createUuid();
    draw::DraftItem first{Fragment, {155, 90}};
    first.fragment.push_back(piece);
    add(first);
    first.a = {180, 90};
    add(first);
    draw::DraftItem table{Table, {155, 110}};
    table.table = draw::DraftTable{2, 68, 6, {QStringLiteral("Item"), QStringLiteral("Value"), QStringLiteral("Wall"), QStringLiteral("4")}};
    add(table);
    draw::DraftItem dimension{Horizontal, {35, 35}, {145, 35}, {80, 24}};
    dimension.dimensionNote = {QStringLiteral("H7"), QStringLiteral("+0.035"), QStringLiteral("0")};
    add(dimension);
    add({Weld, {138, 93}, {}, {149, 81}, QStringLiteral("△4")});
    drawing.items.back().weldReverse = true;
    add({Thread, {100, 68}, {}, {151, 34}, QStringLiteral("M10 × 1.5")});

    const auto file = output.filePath("stage21-drawing.g3draw");
    io::saveDraft(file, drawing);
    const auto reopened = io::openDraft(file);
    check(reopened.items == drawing.items && reopened.paper == drawing.paper
        && reopened.additionalSheets.size() == 1 && reopened.additionalSheets.front().paper.titleBlock == drawing.paper.titleBlock,
          "Draft tools, notation, or custom title blocks did not survive saving");
    check(draw::draftDimensionText(reopened.items[8]) == QStringLiteral("110 H7 +0.035 / 0"),
          "Fit or deviations were lost from the dimension");
    check(draw::DraftGeometry(reopened).resolve(reopened.items[4]).loops == newFill.loops,
          "Hatch associations did not survive saving");
    draw::exportDraftPdf(output.filePath("stage21-drawing.pdf"), reopened);
}

void verifyDxf(const QDir& output)
{
    using enum draw::DraftKind;
    draw::Draft plate;
    plate.layers = {{QStringLiteral("Outline")}, {QStringLiteral("Hidden")},
                    {QStringLiteral("Axes"), true, true}, {QStringLiteral("Construction"), false}};
    plate.items = {{Rectangle, {40, 30}, {140, 90}}, {Circle, {90, 60}, {100, 60}},
                   {Line, {50, 38}, {130, 38}}, {Line, {90, 34}, {90, 87}},
                   {Arc, {118, 58}, {126, 50}, {134, 58}},
                   {Text, {50, 105}, {}, {}, QStringLiteral("Plate Ø20")}};
    plate.items[2].stroke = draw::DraftStroke::Hidden;
    plate.items[2].layer = 1;
    plate.items[3].stroke = draw::DraftStroke::Center;
    plate.items[3].layer = 2;
    draw::DraftItem hatch{Hatch};
    hatch.loops = {{{150, 30}, {180, 30}, {180, 60}, {150, 60}, {150, 30}},
                   {{156, 36}, {164, 36}, {164, 44}, {156, 44}, {156, 36}}};
    plate.items.push_back(hatch);
    plate.items.push_back({Horizontal, {40, 30}, {140, 30}, {90, 20}});

    const auto path = output.filePath("plate-100x60.dxf");
    check(io::exportDxf(path, plate) == 0, "DXF reported a loss for supported drawing items");
    const auto imported = io::importDxf(path);
    const auto& items = imported.drawing.items;
    check(!imported.skipped && !imported.beyondSheet && items.size() == 8, "DXF lost supported items");
    check(items[0].kind == Polyline && items[0].points.front() == items[0].points.back()
        && QLineF(items[0].points[0], items[0].points[1]).length() == 100
        && QLineF(items[0].points[1], items[0].points[2]).length() == 60, "DXF distorted the plate");
    check(items[1].kind == Circle && QLineF(items[1].a, items[1].b).length() == 10, "DXF lost the bore diameter");
    check(items[2].stroke == draw::DraftStroke::Hidden && items[3].stroke == draw::DraftStroke::Center
        && imported.drawing.layers[items[2].layer].name == "Hidden"
        && imported.drawing.layers[items[3].layer].locked, "DXF lost layers or line types");
    check(!imported.drawing.layers.back().visible, "DXF lost hidden layer state");
    check(items[4].kind == Arc && QLineF(items[4].b, plate.items[4].b).length() < 1.e-8
        && items[5].text == plate.items[5].text && items[6].loops == plate.items[6].loops,
        "DXF lost arc, text or hatch");
    check(items[7].kind == Horizontal && QLineF(items[7].a, items[7].b).length() == 100,
          "DXF lost the editable plate width");
    const auto native = output.filePath("plate-from-dxf.g3draw");
    io::saveDraft(native, imported.drawing);
    check(io::openDraft(native).items == items, "Imported geometry could not be saved natively");

    const auto imperial = output.filePath("inch-input.dxf");
    io::writeFile(imperial,
        "0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n70\n1\n0\nENDSEC\n"
        "0\nSECTION\n2\nTABLES\n0\nTABLE\n2\nLAYER\n"
        "0\nLAYER\n2\nAxis\n70\n0\n62\n7\n6\nCENTER\n0\nENDTAB\n0\nENDSEC\n"
        "0\nSECTION\n2\nENTITIES\n"
        "0\nLINE\n8\nAxis\n10\n0\n20\n0\n11\n2\n21\n0\n"
        "0\nLWPOLYLINE\n8\nAxis\n90\n2\n70\n0\n10\n0\n20\n0\n42\n1\n10\n2\n20\n0\n"
        "0\nPOLYLINE\n8\nAxis\n70\n1\n0\nVERTEX\n10\n0\n20\n0\n0\nVERTEX\n10\n1\n20\n0\n"
        "0\nVERTEX\n10\n1\n20\n1\n0\nSEQEND\n0\nINSERT\n8\n0\n0\nENDSEC\n0\nEOF\n");
    const auto scaled = io::importDxf(imperial);
    check(scaled.skipped == 1 && scaled.drawing.items.size() == 3, "DXF legacy polyline or skip count failed");
    const auto& center = scaled.drawing.items.front();
    const auto& arc = scaled.drawing.items[1];
    check(center.stroke == draw::DraftStroke::Center && std::abs(QLineF(center.a, center.b).length() - 50.8) < 1.e-8
        && arc.kind == Arc && std::abs(draw::draftArc(arc.a, arc.b, arc.label).radius - 25.4) < 1.e-8
        && arc.b.y() > arc.a.y(), "Inch conversion, BYLAYER or bulge failed");
}

}  // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        if (args.size() == 3 && args.at(1) == "--verify-external-dxf") {
            const auto imported = io::importDxf(args.at(2));
            const auto& items = imported.drawing.items;
            check(!imported.skipped && items.size() == 7, "External DXF lost supported entities");
            using enum draw::DraftKind;
            const auto plate = std::ranges::find(items, Polyline, &draw::DraftItem::kind);
            const auto bore = std::ranges::find(items, Circle, &draw::DraftItem::kind);
            check(plate != items.end() && plate->points.size() == 5
                && QLineF(plate->points[0], plate->points[1]).length() == 100
                && QLineF(plate->points[1], plate->points[2]).length() == 60
                && bore != items.end() && QLineF(bore->a, bore->b).length() == 10,
                "External DXF changed the plate geometry");
            for (const auto stroke : {draw::DraftStroke::Hidden, draw::DraftStroke::Center}) {
                check(std::ranges::any_of(items, [stroke](const auto& item) {
                    return item.kind == Line && item.stroke == stroke;
                }), "External DXF changed a line type");
            }
            return 0;
        }
        check(args.size() == 2 || args.size() == 3, "Expected examples directory and optional result directory");
        QTemporaryDir temporary;
        check(temporary.isValid(), "Could not create a folder for test drawings");
        const QDir output(args.size() == 3 ? args.at(2) : temporary.path());
        check(output.exists(), "The drawing result directory does not exist");
        using std::numbers::pi;
        const std::array examples{
            Example{"flange", "Mounting flange", "G3D-001", 100, 100, 15,
                    (2500 - 400 - 4 * 25) * 15 * pi, {10, 10, 10, 10, 40, 100}},
            Example{"stepped-bushing", "Stepped bushing", "G3D-002", 40, 40, 30,
                    ((400 - 100) * 10 + (225 - 100) * 20) * pi, {20, 30, 40}},
            Example{"angle-bracket", "Angle bracket", "G3D-003", 80, 45, 45, 52480 - 400 * pi, {10, 10}}
        };
        for (const auto& example : examples) {
            verifyExample(QDir(args.at(1)), output, example);
        }
        verifySections(QDir(args.at(1)), output);
        verifyMultiSheetDrawing(QDir(args.at(1)), output);
        verifyAssemblyDrawing(output);
        verifyParametricBracket(output);
        verifySketchOnFace(output);
        verifyDraft(output);
        verifyAttachedDimensions(output);
        verifyModelViews(output);
        verifyDraftTools(output);
        verifyLinkedDimensions(output);
        verifyDrawingViews(output);
        verifyExtendedDraft(output);
        verifyDxf(output);
        const auto face = geom::makeProfile({geom::Plane::XY, {geom::Rectangle{{0, 0}, {10, 20}}}});
        bool rejected = false;
        try {
            draw::project(face);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "Drawing accepted a face without a solid");
        const auto radial = geom::makeProfile({geom::Plane::XZ, {geom::Rectangle{{10, 0}, {20, 30}}}});
        const auto segment = draw::project(geom::revolve(radial, geom::Plane::XZ, geom::Axis::V, 0, 270));
        check(segment.top.circles.empty(), "A 270 degree arc was mistaken for a complete circle");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
