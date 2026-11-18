#include "geometry/Fastener.h"
#include "io/FileIO.h"
#include "io/ProjectIO.h"
#include "io/DraftIO.h"
#include "io/SpecificationIO.h"

#include <BRepCheck_Analyzer.hxx>
#include <TopExp_Explorer.hxx>
#include <QGuiApplication>
#include <QDir>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace draw = graph3d::drawing;
namespace doc = graph3d::document;
namespace geom = graph3d::geometry;
namespace io = graph3d::io;

namespace {
void check(bool condition, const char* error)
{
    if (!condition) { throw std::runtime_error(error); }
}

void verify(const QDir& project)
{
    using enum geom::FastenerKind;
    doc::Document model;
    geom::Sketch flange{geom::Plane::XY, {geom::Circle{{0, 0}, 50}, geom::Circle{{0, 0}, 14}}};
    constexpr std::array<geom::Point2, 4> holes{{{35, 0}, {0, 35}, {-35, 0}, {0, -35}}};
    for (auto center : holes) { flange.entities.push_back(geom::Circle{center, 4.6}); }
    const auto plate = model.addExtrude(model.addSketch(flange), 12);
    model.setPartProperties(plate, {"FL-100", "Mounting flange", "Steel", "Four clearance holes"});
    const auto bolt1 = model.addFastener({Bolt, 8, 25});
    const auto bolt2 = model.addFastener({Bolt, 8, 25});
    const auto longer = model.addFastener({Bolt, 8, 35});
    const auto nut = model.addFastener({Nut, 8, 0});
    const auto washer = model.addFastener({Washer, 8, 0});
    for (const auto& size : geom::fastenerCatalog()) {
        for (const auto kind : {Bolt, Nut, Washer}) {
            const auto shape = geom::makeFastener({kind, size.diameter, kind == Bolt ? size.lengths.front() : 0});
            check(BRepCheck_Analyzer(shape).IsValid() && TopExp_Explorer(shape, TopAbs_SOLID).More(), "Invalid catalog solid");
        }
    }
    doc::Assembly placement{{{plate, {}, true}}};
    for (int i = 0; i < 4; ++i) {
        placement.components.push_back({i % 2 ? bolt2 : bolt1, {0, {holes[i].u, holes[i].v, -8}, {}}});
    }
    placement.components.push_back({nut, {0, {35, 0, -7}, {}}});
    placement.components.push_back({washer, {0, {35, 0, 12}, {}}});
    const auto assembly = model.addAssembly(placement);
    model.setPartProperties(assembly, {"FL-100-A", "Flange and mounting hardware", {}, {}});
    model.setVisible(longer, false);
    doc::Configuration vars;
    vars.variables = {{"height", "12"}};
    vars.bindings[plate] = {{"distance", "height"}};
    vars.variants["Thick"] = {{"height", "15"}};
    model.setConfiguration(vars);
    const auto modelPath = project.filePath("flange-fasteners.graph3d");
    const auto drawPath = project.filePath("flange-fasteners.g3draw");
    const auto specPath = project.filePath("flange-fasteners.g3spec");
    io::saveProject(modelPath, model);
    auto source = io::openModelSource(modelPath);
    check(source.document.find(assembly)->status == doc::Status::Ready, "Saved catalog assembly does not rebuild");
    const auto initial = io::buildModelViews(source, assembly, {}, {297, 210}, nullptr, QString("Thick"));
    check(initial.assembly && initial.assembly->rows.size() == 4, "Catalog fastener families were not consolidated");
    const auto byName = [](const auto& rows, const QString& designation) -> const auto& {
        const auto it = std::ranges::find_if(rows, [&](const auto& row) { return row.designation == designation; });
        if (it == rows.end()) { throw std::runtime_error("Missing designation in the table"); }
        return *it;
    };
    const auto& oldBoltRow = byName(initial.assembly->rows, "G3D-B M8x25");
    check(oldBoltRow.instances.size() == 4 && oldBoltRow.standard, "Identical catalog bolts did not group");
    const int originalPosition = oldBoltRow.position;

    draw::SpecificationDocument spec;
    spec.sourcePath = modelPath;
    spec.assemblyId = assembly;
    spec.variant = QString("Thick");
    spec.drawingPath = drawPath;
    spec.nextPosition = initial.assembly->nextPosition;
    spec.history = initial.assembly->positionHistory;
    draw::mergeSpecification(spec, *io::readSpecificationAssembly(spec));
    auto row = std::ranges::find(spec.rows, originalPosition, &draw::SpecRow::position);
    row->section = "Hardware";
    row->note = "Keep; \"finish\"";
    spec.rows.push_back({{}, "Other items", "MAN-01", "Grease", {}, "=manual", spec.nextPosition++, 1});
    io::saveSpecification(specPath, spec);
    check(io::openSpecification(specPath) == spec, "Standalone specification or variant did not survive reopening");

    auto linked = initial;
    io::attachSpecification(linked, specPath);
    auto markers = std::make_shared<draw::AssemblyDrawing>(*linked.assembly);
    for (const auto& family : markers->rows) {
        for (std::size_t i = 0; i < family.componentSlots.size(); ++i) {
            const auto index = static_cast<int>(i);
            const auto number = family.componentSlots[i];
            markers->callouts.push_back({family.position, index, 2, {}, {210. + 4*number, 150. - 6*number}, number});
        }
    }
    linked.assembly = markers;
    draw::Draft draft;
    draft.modelViews = linked;
    io::saveDraft(drawPath, draft);
    auto saved = io::openDraft(drawPath);
    check(saved.modelViews->specificationPath == specPath
        && draw::auditSpecification(spec, io::readSpecificationAssembly(spec).get(), &*saved.modelViews).empty(),
        "Initial drawing and table do not agree");

    placement.components[1].body = longer;
    placement.components[2].body = longer;
    check(model.editAssembly(assembly, placement), "Could not replace two bolts");
    io::saveProject(modelPath, model);
    const auto current = io::readSpecificationAssembly(spec);
    auto report = draw::auditSpecification(spec, current.get(), &*saved.modelViews);
    check(report.join("; ").contains("model quantity 2, table quantity 4") && report.join("; ").contains("M8x35"),
        "Stale quantities or missing new part were not reported");
    spec = io::openSpecification(specPath);
    draw::mergeSpecification(spec, *current);
    const auto& shortBolt = byName(spec.rows, "G3D-B M8x25");
    const auto& longBolt = byName(spec.rows, "G3D-B M8x35");
    check(shortBolt.position == originalPosition && shortBolt.quantity == 2 && shortBolt.section == "Hardware"
        && shortBolt.note == "Keep; \"finish\"" && longBolt.position != originalPosition && longBolt.quantity == 2,
        "Replacement lost stable numbering, quantities or manual row metadata");
    const auto newPosition = longBolt.position;
    check(byName(spec.rows, "MAN-01").quantity == 1, "Manual item did not survive refresh");
    io::saveSpecification(specPath, spec);
    source = io::openModelSource(modelPath);
    saved.modelViews = io::buildModelViews(source, assembly, {}, {297, 210}, &*saved.modelViews);
    for (const auto& marker : saved.modelViews->assembly->callouts) {
        if (marker.componentSlot == 2 || marker.componentSlot == 3) {
            check(marker.position == newPosition, "A replacement drawing label did not follow its instance");
        }
        if (marker.componentSlot == 4 || marker.componentSlot == 5) {
            check(marker.position == originalPosition, "A surviving part was renumbered");
        }
    }
    check(saved.modelViews->assembly->callouts.size() == 7, "Drawing callouts were lost after replacement");
    check(draw::auditSpecification(spec, current.get(), &*saved.modelViews).empty(), "Updated documents still disagree");
    io::saveDraft(drawPath, saved);
    const auto pdf = project.filePath("flange-fasteners-spec.pdf");
    draw::exportSpecificationPdf(pdf, spec);
    const auto data = io::readFile(pdf);
    check(data.startsWith("%PDF-") && data.count("/Type /Page\n") == 1, "Standalone PDF was not generated");
    const auto csv = draw::specificationCsv(spec);
    check(csv.startsWith("\xef\xbb\xbf") && csv.contains("\"Keep; \"\"finish\"\"\"") && csv.contains("'=manual")
        && csv.contains("G3D-B M8x25") && csv.contains("G3D-B M8x35"), "CSV lost values or escaping");
    io::writeFile(project.filePath("flange-fasteners-spec.csv"), csv);

    auto onlyLong = *current;
    std::erase_if(onlyLong.rows, [](const auto& line) { return line.designation == "G3D-B M8x25"; });
    auto history = spec;
    draw::mergeSpecification(history, onlyLong);
    draw::mergeSpecification(history, *current);
    check(byName(history.rows, "G3D-B M8x25").position == originalPosition, "Returning a part forgot its reserved position");
}
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir tmp;
    try {
        QDir output(argc == 2 ? app.arguments().at(1) : tmp.path());
        check(output.exists(), "Could not open the test output folder");
        verify(output);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
