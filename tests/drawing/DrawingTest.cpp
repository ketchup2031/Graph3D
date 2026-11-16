#include "drawing/DrawingSheet.h"
#include "io/DraftIO.h"
#include "io/ModelViewsIO.h"
#include "geometry/SketchModeling.h"
#include "io/ProjectIO.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
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

}  // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        const auto args = app.arguments();
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
        verifyDraft(output);
        verifyAttachedDimensions(output);
        verifyModelViews(output);
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
