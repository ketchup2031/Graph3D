#include "drawing/DrawingSheet.h"
#include "geometry/SketchModeling.h"
#include "io/ProjectIO.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
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
    double x, y, z, volume;
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

    const auto restoredFile = output.filePath(name + "-roundtrip.graph3d");
    io::saveProject(restoredFile, model);
    const auto reopened = io::openProject(restoredFile);
    const auto newViews = draw::project(finishedPart(reopened).shape);
    checkView(newViews.front, example.x, example.z);
    checkView(newViews.top, example.x, example.y);
    checkView(newViews.right, example.y, example.z);

    const auto pdf = output.filePath(name + ".pdf");
    draw::savePdf(pdf, views, {QString::fromLatin1(example.title), QStringLiteral("G3D-001"), true, true});
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
            Example{"flange", "Mounting flange", 100, 100, 15, (2500 - 400 - 4 * 25) * 15 * pi},
            Example{"stepped-bushing", "Stepped bushing", 40, 40, 30,
                    ((400 - 100) * 10 + (225 - 100) * 20) * pi},
            Example{"angle-bracket", "Angle bracket", 80, 45, 45, 52480 - 400 * pi}
        };
        for (const auto& example : examples) {
            verifyExample(QDir(args.at(1)), output, example);
        }
        const auto face = geom::makeProfile({geom::Plane::XY, {geom::Rectangle{{0, 0}, {10, 20}}}});
        bool rejected = false;
        try {
            draw::project(face);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "Drawing accepted a face without a solid");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
