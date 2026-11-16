#include "drawing/DrawingSheet.h"
#include "geometry/SketchModeling.h"
#include "io/ProjectIO.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
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
