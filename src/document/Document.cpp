#include "document/Document.h"

#include "geometry/Primitives.h"
#include "geometry/SketchModeling.h"

#include <BRepCheck_Analyzer.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace graph3d::document {
namespace {

template<class... Functions>
struct Visit : Functions... { using Functions::operator()...; };

std::vector<ObjectId> inputs(const Parameters& parameters)
{
    return std::visit(Visit{
        [](const Assembly& p) {
            std::vector<ObjectId> ids;
            ids.reserve(p.components.size());
            for (const auto& component : p.components) {
                ids.push_back(component.body);
            }
            return ids;
        },
        [](const Extrude& p) { return std::vector{p.profile}; },
        [](const Revolve& p) { return std::vector{p.profile}; },
        [](const Boolean& p) { return std::vector{p.first, p.second}; },
        [](const Hole& p) { return std::vector{p.body}; },
        [](const EdgeTreatment& p) { return std::vector{p.body}; },
        [](const Transform& p) { return std::vector{p.body}; },
        [](const auto&) { return std::vector<ObjectId>{}; }
    }, parameters);
}

const Object& objectAt(const std::vector<Object>& state, ObjectId id)
{
    if (id < 1 || static_cast<std::size_t>(id) > state.size()) {
        throw std::invalid_argument("The requested object does not exist.");
    }
    return state[id - 1];
}

void requireBody(const Object& object)
{
    if (std::holds_alternative<geometry::Sketch>(object.parameters)) {
        throw std::invalid_argument("Select a solid body.");
    }
}

const geometry::Sketch& requireSketch(const Object& object)
{
    const auto* sketch = std::get_if<geometry::Sketch>(&object.parameters);
    if (!sketch) {
        throw std::invalid_argument("Select a finished sketch.");
    }
    return *sketch;
}

TopoDS_Shape evaluate(const std::vector<Object>& state, const Parameters& parameters)
{
    return std::visit(Visit{
        [](const Primitive& p) -> TopoDS_Shape { return geometry::makePrimitive(p.kind, p.x, p.y); },
        [](const geometry::Sketch& p) -> TopoDS_Shape { return geometry::makeProfile(p); },
        [&state](const Extrude& p) {
            const auto& profile = objectAt(state, p.profile);
            return geometry::extrude(TopoDS::Face(profile.shape), requireSketch(profile).plane, p.distance);
        },
        [&state](const Revolve& p) {
            const auto& profile = objectAt(state, p.profile);
            return geometry::revolve(TopoDS::Face(profile.shape), requireSketch(profile).plane,
                                     p.axis, p.offset, p.angle);
        },
        [&state](const Boolean& p) {
            return geometry::booleanResult(objectAt(state, p.first).shape,
                                           objectAt(state, p.second).shape, p.operation);
        },
        [&state](const Hole& p) {
            return geometry::drillHole(objectAt(state, p.body).shape, p.face, p.size);
        },
        [&state](const EdgeTreatment& p) {
            return geometry::treatEdge(objectAt(state, p.body).shape, p.edge, p.kind, p.size);
        },
        [&state](const Transform& p) {
            return geometry::transformBody(objectAt(state, p.body).shape, p.parameters);
        },
        [&state](const Assembly& p) {
            std::vector<geometry::PlacedBody> bodies;
            bodies.reserve(p.components.size());
            for (const auto& component : p.components) {
                bodies.push_back({objectAt(state, component.body).shape, component.placement});
            }
            return geometry::makeAssembly(bodies);
        },
        [](const Imported& imported) {
            if (imported.sourceShape.IsNull() || !TopExp_Explorer(imported.sourceShape, TopAbs_SOLID).More()
                || !BRepCheck_Analyzer(imported.sourceShape).IsValid()) {
                throw std::invalid_argument("The imported geometry is not a valid solid.");
            }
            return imported.sourceShape;
        }
    }, parameters);
}

}  // namespace

ObjectId Document::addPrimitive(geometry::Primitive kind)
{
    constexpr double spacing = 140;
    constexpr std::size_t columns = 5;
    const auto index = objects_.size();
    return append(Primitive{kind, spacing * static_cast<double>(index % columns),
                                  spacing * static_cast<double>(index / columns)});
}

ObjectId Document::addSketch(geometry::Sketch sketch)
{
    return append(std::move(sketch));
}

ObjectId Document::addExtrude(ObjectId profile, double distance)
{
    requireSketch(objectAt(objects_, profile));
    return append(Extrude{profile, distance});
}

ObjectId Document::addRevolve(ObjectId profile, geometry::Axis axis, double offset, double angle)
{
    requireSketch(objectAt(objects_, profile));
    return append(Revolve{profile, axis, offset, angle});
}

ObjectId Document::addBoolean(ObjectId first, ObjectId second, geometry::BooleanOperation operation)
{
    if (first == second) {
        throw std::invalid_argument("Choose two different bodies.");
    }
    for (const auto id : {first, second}) {
        if (std::holds_alternative<geometry::Sketch>(objectAt(objects_, id).parameters)) {
            throw std::invalid_argument("Choose bodies rather than sketches.");
        }
    }
    return append(Boolean{first, second, operation});
}

ObjectId Document::addImported(std::string name, TopoDS_Shape shape)
{
    return append(Imported{std::move(name), std::move(shape)});
}

ObjectId Document::addHole(ObjectId body, geometry::FaceReference face, geometry::HoleSize size)
{
    requireBody(objectAt(objects_, body));
    return append(Hole{body, face, size});
}

ObjectId Document::addEdgeTreatment(ObjectId body, geometry::EdgeReference edge,
                                    geometry::EdgeTreatmentKind kind, double size)
{
    requireBody(objectAt(objects_, body));
    return append(EdgeTreatment{body, edge, kind, size});
}

ObjectId Document::addTransform(ObjectId body, geometry::BodyTransform parameters)
{
    requireBody(objectAt(objects_, body));
    return append(Transform{body, std::move(parameters)});
}

ObjectId Document::addAssembly(Assembly assembly)
{
    for (const auto& component : assembly.components) {
        requireBody(objectAt(objects_, component.body));
    }
    return append(std::move(assembly));
}

Document Document::fromDefinitions(std::vector<Definition> definitions)
{
    Document document;
    auto& records = document.objects_;
    records.reserve(definitions.size());
    for (auto& definition : definitions) {
        const auto id = static_cast<ObjectId>(records.size() + 1);
        const auto& parameters = definition.parameters;
        const auto requirePrior = [&records](ObjectId source) -> const Object& {
            return objectAt(records, source);
        };
        if (const auto* extrusion = std::get_if<Extrude>(&parameters)) {
            requireSketch(requirePrior(extrusion->profile));
        } else if (const auto* revolution = std::get_if<Revolve>(&parameters)) {
            requireSketch(requirePrior(revolution->profile));
        } else if (const auto* hole = std::get_if<Hole>(&parameters)) {
            requireBody(requirePrior(hole->body));
        } else if (const auto* edge = std::get_if<EdgeTreatment>(&parameters)) {
            requireBody(requirePrior(edge->body));
        } else if (const auto* transform = std::get_if<Transform>(&parameters)) {
            requireBody(requirePrior(transform->body));
        } else if (const auto* assembly = std::get_if<Assembly>(&parameters)) {
            for (const auto& component : assembly->components) {
                requireBody(requirePrior(component.body));
            }
        } else if (const auto* boolean = std::get_if<Boolean>(&parameters)) {
            if (boolean->first == boolean->second
                || std::holds_alternative<geometry::Sketch>(requirePrior(boolean->first).parameters)
                || std::holds_alternative<geometry::Sketch>(requirePrior(boolean->second).parameters)) {
                throw std::invalid_argument("Invalid references in a boolean operation.");
            }
        }
        records.push_back({id, std::move(definition.parameters), {}, Status::Ready, {}, definition.visible});
    }
    rebuild(records, 0, true);
    for (const auto& object : records) {
        if (object.status != Status::Ready
            && (std::holds_alternative<Primitive>(object.parameters)
                || std::holds_alternative<geometry::Sketch>(object.parameters)
                || std::holds_alternative<Imported>(object.parameters))) {
            throw std::invalid_argument("Object #" + std::to_string(object.id) + ": " + object.error);
        }
    }
    return document;
}

ObjectId Document::append(Parameters parameters)
{
    auto next = objects_;
    const auto id = static_cast<ObjectId>(next.size() + 1);
    next.push_back({id, std::move(parameters), {}, Status::Ready, {}, true});
    rebuild(next, next.size() - 1);
    if (next.back().status != Status::Ready) {
        throw std::invalid_argument(next.back().error);
    }
    for (const auto input : inputs(next.back().parameters)) {
        next[input - 1].visible = false;
    }
    commit(std::move(next));
    return id;
}

bool Document::editExtrude(ObjectId id, double distance)
{
    const auto* operation = std::get_if<Extrude>(&objectAt(objects_, id).parameters);
    if (!operation) {
        throw std::invalid_argument("Select an Extrude operation.");
    }
    if (operation->distance == distance) {
        return false;
    }
    auto next = objects_;
    std::get<Extrude>(next[id - 1].parameters).distance = distance;
    rebuild(next, id - 1);
    commit(std::move(next));
    return true;
}

bool Document::editRevolve(ObjectId id, geometry::Axis axis, double offset, double angle)
{
    const auto* operation = std::get_if<Revolve>(&objectAt(objects_, id).parameters);
    if (!operation) {
        throw std::invalid_argument("Select a Revolve operation.");
    }
    if (operation->axis == axis && operation->offset == offset && operation->angle == angle) {
        return false;
    }
    auto next = objects_;
    next[id - 1].parameters = Revolve{operation->profile, axis, offset, angle};
    rebuild(next, id - 1);
    commit(std::move(next));
    return true;
}

bool Document::editHole(ObjectId id, geometry::HoleSize size)
{
    const auto* operation = std::get_if<Hole>(&objectAt(objects_, id).parameters);
    if (!operation) {
        throw std::invalid_argument("Select a Hole operation.");
    }
    if (operation->size.diameter == size.diameter && operation->size.depth == size.depth
        && operation->size.through == size.through) {
        return false;
    }
    auto next = objects_;
    std::get<Hole>(next[id - 1].parameters).size = size;
    rebuild(next, id - 1);
    commit(std::move(next));
    return true;
}

bool Document::editEdgeTreatment(ObjectId id, double size)
{
    const auto* operation = std::get_if<EdgeTreatment>(&objectAt(objects_, id).parameters);
    if (!operation) {
        throw std::invalid_argument("Select a Chamfer or Fillet operation.");
    }
    if (operation->size == size) {
        return false;
    }
    auto next = objects_;
    std::get<EdgeTreatment>(next[id - 1].parameters).size = size;
    rebuild(next, id - 1);
    commit(std::move(next));
    return true;
}

bool Document::editTransform(ObjectId id, geometry::BodyTransform parameters)
{
    const auto* operation = std::get_if<Transform>(&objectAt(objects_, id).parameters);
    if (!operation || operation->parameters.index() != parameters.index()) {
        throw std::invalid_argument("Select a matching array or mirror operation.");
    }
    if (operation->parameters == parameters) {
        return false;
    }
    auto next = objects_;
    std::get<Transform>(next[id - 1].parameters).parameters = std::move(parameters);
    rebuild(next, id - 1);
    commit(std::move(next));
    return true;
}

bool Document::editAssembly(ObjectId id, Assembly assembly)
{
    const auto* existing = std::get_if<Assembly>(&objectAt(objects_, id).parameters);
    if (!existing || !std::ranges::equal(existing->components, assembly.components, {},
                                         &Component::body, &Component::body)) {
        throw std::invalid_argument("Edit the placement of the existing assembly components.");
    }
    if (existing->components == assembly.components) {
        return false;
    }
    auto next = objects_;
    next[id - 1].parameters = std::move(assembly);
    rebuild(next, id - 1);
    commit(std::move(next));
    return true;
}

void Document::rebuild(State& state, std::size_t first, bool all)
{
    if (state.empty()) {
        return;
    }
    std::vector<bool> changed(state.size(), all);
    changed[first] = true;
    for (std::size_t i = first; i < state.size(); ++i) {
        auto& object = state[i];
        const auto source = inputs(object.parameters);
        for (const auto id : source) {
            if (changed[id - 1]) {
                changed[i] = true;
            }
        }
        if (!changed[i]) {
            continue;
        }
        object.shape.Nullify();
        object.status = Status::Ready;
        object.error.clear();
        for (const auto id : source) {
            if (state[id - 1].status != Status::Ready) {
                object.status = Status::Blocked;
                object.error = "Input #" + std::to_string(id) + " has an error. Fix it to rebuild this operation.";
                break;
            }
        }
        if (object.status == Status::Blocked) {
            continue;
        }
        try {
            object.shape = evaluate(state, object.parameters);
        } catch (const Standard_Failure& error) {
            object.status = Status::Failed;
            const auto* message = error.GetMessageString();
            object.error = message && *message ? message : "Open CASCADE could not build this object.";
        } catch (const std::exception& error) {
            object.status = Status::Failed;
            object.error = error.what();
        }
    }
}

bool Document::setVisible(ObjectId id, bool visible)
{
    if (objectAt(objects_, id).visible == visible) {
        return false;
    }
    auto next = objects_;
    next[id - 1].visible = visible;
    commit(std::move(next));
    return true;
}

bool Document::clear()
{
    if (objects_.empty()) {
        return false;
    }
    commit({});
    return true;
}

void Document::commit(State next)
{
    undo_.push_back({std::move(objects_), revision_});
    objects_ = std::move(next);
    revision_ = nextRevision_++;
    redo_.clear();
}

bool Document::undo()
{
    if (!canUndo()) {
        return false;
    }
    redo_.push_back({std::move(objects_), revision_});
    revision_ = undo_.back().revision;
    objects_ = std::move(undo_.back().objects);
    undo_.pop_back();
    return true;
}

bool Document::redo()
{
    if (!canRedo()) {
        return false;
    }
    undo_.push_back({std::move(objects_), revision_});
    revision_ = redo_.back().revision;
    objects_ = std::move(redo_.back().objects);
    redo_.pop_back();
    return true;
}

void Document::clearHistory()
{
    undo_.clear();
    redo_.clear();
}

const Object* Document::find(ObjectId id) const
{
    if (id < 1 || static_cast<std::size_t>(id) > objects_.size()) {
        return nullptr;
    }
    return &objects_[id - 1];
}

}  // namespace graph3d::document
