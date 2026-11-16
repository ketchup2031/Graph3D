#include "document/Document.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>

namespace doc = graph3d::document;
namespace geom = graph3d::geometry;

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

double volume(const doc::Document& document, doc::ObjectId id)
{
    const auto* object = document.find(id);
    check(object && !object->shape.IsNull() && object->status == doc::Status::Ready, "Missing valid shape");
    GProp_GProps properties;
    BRepGProp::VolumeProperties(object->shape, properties);
    return properties.Mass();
}

void volumeIs(const doc::Document& document, doc::ObjectId id, double expected)
{
    const double result = volume(document, id);
    if (std::abs(result - expected) > expected * 1e-7) {
        throw std::runtime_error("Unexpected volume: " + std::to_string(result)
                                 + ", expected " + std::to_string(expected));
    }
}

void dependencyAndHistory()
{
    doc::Document document;
    const auto sketch = document.addSketch({geom::Plane::XY, {geom::Rectangle{{0, 0}, {20, 20}}}});
    const auto extrusion = document.addExtrude(sketch, 10);
    const auto secondSketch = document.addSketch({geom::Plane::XY, {geom::Rectangle{{10, 0}, {30, 20}}}});
    const auto secondExtrusion = document.addExtrude(secondSketch, 20);
    const auto independent = document.addPrimitive(geom::Primitive::Cube);
    const auto common = document.addBoolean(extrusion, secondExtrusion, geom::BooleanOperation::Intersection);
    const auto unionId = document.addBoolean(common, independent, geom::BooleanOperation::Union);
    check(!document.find(sketch)->visible, "Source profile should be hidden");
    check(!document.find(extrusion)->visible, "Source solid should be hidden after boolean");
    const auto unaffectedShape = document.find(independent)->shape;
    const auto secondShape = document.find(secondExtrusion)->shape;
    const auto profileShape = document.find(sketch)->shape;

    volumeIs(document, common, 2000);
    volumeIs(document, unionId, 1'002'000);
    const auto commonBefore = document.find(common)->shape;

    check(document.editExtrude(extrusion, 30), "Extrude edit was not recorded");
    volumeIs(document, extrusion, 12'000);
    volumeIs(document, common, 4000);
    volumeIs(document, unionId, 1'004'000);
    check(!document.find(common)->shape.IsSame(commonBefore), "Dependent geometry did not rebuild");
    check(document.find(independent)->shape.IsSame(unaffectedShape)
          && document.find(secondExtrusion)->shape.IsSame(secondShape)
          && document.find(sketch)->shape.IsSame(profileShape), "Unrelated geometry was rebuilt");

    check(document.editExtrude(extrusion, 0), "Invalid settings should be retained for correction");
    check(document.find(extrusion)->status == doc::Status::Failed && document.find(extrusion)->shape.IsNull(),
          "Invalid extrusion retained stale geometry");
    check(document.find(common)->status == doc::Status::Blocked && document.find(common)->shape.IsNull(),
          "Dependent common remained valid with a broken input");
    check(document.find(unionId)->status == doc::Status::Blocked && document.find(unionId)->shape.IsNull(),
          "Transitive dependency remained valid with a broken input");
    check(document.undo(), "Undo failed");
    volumeIs(document, unionId, 1'004'000);
    check(document.redo(), "Redo failed");
    check(document.find(extrusion)->status == doc::Status::Failed, "Redo did not restore invalid settings");
    check(document.editExtrude(extrusion, 5), "Invalid extrusion could not be repaired");
    volumeIs(document, unionId, 1'001'000);
    check(document.find(common)->error.empty() && document.find(unionId)->error.empty(), "Stale errors remain after rebuilding");
    check(!document.editExtrude(extrusion, 5), "Unchanged values created a spurious command");

    check(document.undo() && document.canRedo(), "Expected a redo branch");
    check(document.editExtrude(extrusion, 15) && !document.canRedo(), "A new change did not discard the redo branch");
    volumeIs(document, unionId, 1'003'000);

    check(document.clear() && document.objects().empty(), "Clear failed");
    check(document.undo() && document.find(unionId), "Undo did not restore the document");
    volumeIs(document, unionId, 1'003'000);
}

void revolutionAndRejectedCreation()
{
    doc::Document document;
    const auto sketch = document.addSketch({geom::Plane::XZ, {geom::Rectangle{{10, 0}, {20, 30}}}});
    const auto revolved = document.addRevolve(sketch, geom::Axis::V, 0, 360);
    volumeIs(document, revolved, 9000 * std::numbers::pi);
    check(document.editRevolve(revolved, geom::Axis::V, 5, 90), "Revolve edit failed");
    volumeIs(document, revolved, 1500 * std::numbers::pi);
    try {
        document.addSketch({geom::Plane::XY, {geom::Line{{0, 0}, {5, 0}}}});
        throw std::runtime_error("Open sketch was inserted into the document");
    } catch (const std::invalid_argument&) {
        check(document.objects().size() == 2, "Failed creation changed the document");
    }
    check(document.undo(), "Revolve edit could not be undone");
    volumeIs(document, revolved, 9000 * std::numbers::pi);
    check(document.redo(), "Revolve edit could not be redone");
    volumeIs(document, revolved, 1500 * std::numbers::pi);
    check(document.setVisible(sketch, true) && document.find(sketch)->visible, "Visibility command failed");
    check(document.undo() && !document.find(sketch)->visible, "Visibility undo failed");
}

}  // namespace

int main()
{
    try {
        dependencyAndHistory();
        revolutionAndRejectedCreation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
