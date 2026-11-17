#include "document/Document.h"

#include "geometry/Primitives.h"
#include "geometry/SketchConstraints.h"

#include <BRepCheck_Analyzer.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
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
        [](const geometry::Sketch& p) {
            return p.support ? std::vector{p.support->body} : p.datumPlane ? std::vector{p.datumPlane} : std::vector<ObjectId>{};
        },
        [](const Extrude& p) { return std::vector{p.profile}; },
        [](const Revolve& p) { return p.datumAxis ? std::vector{p.profile, p.datumAxis} : std::vector{p.profile}; },
        [](const Sweep& p) { return std::vector{p.profile}; },
        [](const Loft& p) { return p.profiles; },
        [](const Shell& p) { return std::vector{p.body}; },
        [](const DraftAngle& p) { return std::vector{p.body}; },
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
    if (!isBody(object.parameters)) {
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

geometry::SketchContext resolveContext(const std::vector<Object>& state, const geometry::Sketch& sketch)
{
    if (sketch.datumPlane) {
        if (sketch.support) {
            throw std::invalid_argument("A sketch can use only one support.");
        }
        const auto& datum = objectAt(state, sketch.datumPlane);
        const auto* plane = std::get_if<geometry::DatumPlane>(&datum.parameters);
        if (!plane || datum.status != Status::Ready) {
            throw std::invalid_argument("Select a valid construction plane.");
        }
        return {geometry::datumFrame(*plane), {}};
    }
    if (!sketch.support) {
        return geometry::sketchContext(sketch);
    }
    const auto& body = objectAt(state, sketch.support->body);
    requireBody(body);
    if (body.status != Status::Ready) {
        throw std::invalid_argument("The sketch support body has an error.");
    }
    return geometry::sketchContext(sketch, body.shape);
}

void validateAdvancedInputs(const std::vector<Object>& state, const Parameters& parameters, ObjectId id)
{
    const auto prior = [&](ObjectId source) -> const Object& {
        if (source >= id) {
            throw std::invalid_argument("An input must precede its operation.");
        }
        return objectAt(state, source);
    };
    const auto validateAxis = [&](ObjectId source) {
        if (source && !std::holds_alternative<geometry::DatumAxis>(prior(source).parameters)) {
            throw std::invalid_argument("Select a construction axis.");
        }
    };
    std::visit(Visit{
        [&](const geometry::Sketch& p) {
            if (p.datumPlane && (p.support || !std::holds_alternative<geometry::DatumPlane>(prior(p.datumPlane).parameters))) {
                throw std::invalid_argument("Select a valid construction plane.");
            }
            if (p.support) {
                requireBody(prior(p.support->body));
            }
        },
        [&](const Revolve& p) { validateAxis(p.datumAxis); },
        [&](const Sweep& p) { requireSketch(prior(p.profile)); },
        [&](const Loft& p) {
            if (p.profiles.size() < 2 || p.profiles.size() > 32) {
                throw std::invalid_argument("Select two to 32 ordered loft sections.");
            }
            std::set<ObjectId> ids;
            for (auto profile : p.profiles) {
                requireSketch(prior(profile));
                if (!ids.insert(profile).second) {
                    throw std::invalid_argument("Select each loft section once.");
                }
            }
        },
        [&](const Shell& p) { requireBody(prior(p.body)); },
        [&](const DraftAngle& p) { requireBody(prior(p.body)); },
        [](const auto&) {}
    }, parameters);
}

std::vector<geometry::PlacedBody> placedBodies(const std::vector<Object>& state, const Assembly& assembly)
{
    std::vector<geometry::PlacedBody> bodies;
    bodies.reserve(assembly.components.size());
    for (const auto& component : assembly.components) {
        bodies.push_back({objectAt(state, component.body).shape, component.placement, component.fixed});
    }
    return bodies;
}

TopoDS_Shape evaluate(const std::vector<Object>& state, const Parameters& parameters)
{
    return std::visit(Visit{
        [](const Primitive& p) -> TopoDS_Shape { return geometry::makePrimitive(p.kind, p.x, p.y); },
        [&state](const geometry::Sketch& p) -> TopoDS_Shape { return geometry::makeProfile(p, resolveContext(state, p)); },
        [&state](const Extrude& p) {
            const auto& profile = objectAt(state, p.profile);
            return geometry::extrude(TopoDS::Face(profile.shape), resolveContext(state, requireSketch(profile)).frame, p.distance);
        },
        [&state](const Revolve& p) {
            const auto& profile = objectAt(state, p.profile);
            auto frame = resolveContext(state, requireSketch(profile)).frame;
            if (p.datumAxis) {
                frame = geometry::rotationFrame(std::get<geometry::DatumAxis>(objectAt(state, p.datumAxis).parameters), frame);
                return geometry::revolve(TopoDS::Face(profile.shape), frame, geometry::Axis::U, 0, p.angle);
            }
            return geometry::revolve(TopoDS::Face(profile.shape), frame, p.axis, p.offset, p.angle);
        },
        [&state](const Boolean& p) {
            return geometry::booleanResult(objectAt(state, p.first).shape,
                                           objectAt(state, p.second).shape, p.operation);
        },
        [&state](const Hole& p) {
            return geometry::drillHole(objectAt(state, p.body).shape, p.face, p.size);
        },
        [&state](const EdgeTreatment& p) {
            auto edges = p.additionalEdges;
            edges.insert(edges.begin(), p.edge);
            return geometry::treatEdges(objectAt(state, p.body).shape, edges, p.kind, p.size);
        },
        [&state](const Transform& p) {
            return geometry::transformBody(objectAt(state, p.body).shape, p.parameters);
        },
        [&state](const Assembly& p) { return geometry::makeAssembly(placedBodies(state, p), p.mates); },
        [](const LinkedComponent& p) {
            if (p.snapshot.IsNull() || !TopExp_Explorer(p.snapshot, TopAbs_SOLID).More()
                || !BRepCheck_Analyzer(p.snapshot).IsValid()) {
                throw std::invalid_argument("The linked component has no valid saved geometry.");
            }
            return p.snapshot;
        },
        [](const geometry::DatumPlane& p) { return geometry::datumOutline(p); },
        [](const geometry::DatumAxis& p) { return geometry::datumOutline(p); },
        [&state](const Sweep& p) {
            const auto& sketch = objectAt(state, p.profile);
            return geometry::sweepProfile(TopoDS::Face(sketch.shape), resolveContext(state, requireSketch(sketch)).frame, p.path);
        },
        [&state](const Loft& p) {
            std::vector<TopoDS_Face> sections;
            for (const auto id : p.profiles) {
                sections.push_back(TopoDS::Face(objectAt(state, id).shape));
            }
            return geometry::loftProfiles(sections, p.ruled);
        },
        [&state](const Shell& p) { return geometry::shellBody(objectAt(state, p.body).shape, p.openings, p.thickness); },
        [&state](const DraftAngle& p) { return geometry::draftBody(objectAt(state, p.body).shape, p.neutral, p.walls, p.degrees); },
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

bool isBody(const Parameters& parameters)
{
    return !std::holds_alternative<geometry::Sketch>(parameters)
        && !std::holds_alternative<geometry::DatumPlane>(parameters)
        && !std::holds_alternative<geometry::DatumAxis>(parameters);
}

bool isAdvanced(const Parameters& parameters)
{
    return std::holds_alternative<geometry::DatumPlane>(parameters) || std::holds_alternative<geometry::DatumAxis>(parameters)
        || std::holds_alternative<Sweep>(parameters) || std::holds_alternative<Loft>(parameters)
        || std::holds_alternative<Shell>(parameters) || std::holds_alternative<DraftAngle>(parameters);
}

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
    if (sketch.support) {
        requireBody(objectAt(objects_, sketch.support->body));
    }
    return append(std::move(sketch));
}

geometry::SketchContext Document::sketchContext(const geometry::Sketch& sketch) const
{
    return resolveContext(objects_, sketch);
}

bool Document::editSketch(ObjectId id, geometry::Sketch sketch)
{
    const auto& original = requireSketch(objectAt(objects_, id));
    if (original == sketch) {
        return false;
    }
    validateAdvancedInputs(objects_, Parameters(sketch), id);
    if (sketch.support) {
        if (sketch.support->body >= id) {
            throw std::invalid_argument("A sketch support must precede the sketch.");
        }
        requireBody(objectAt(objects_, sketch.support->body));
    }
    auto next = objects_;
    next[id - 1].parameters = std::move(sketch);
    rebuild(next, id - 1);
    if (next[id - 1].status != Status::Ready) {
        throw std::invalid_argument(next[id - 1].error);
    }
    commit(std::move(next));
    return true;
}

ObjectId Document::addExtrude(ObjectId profile, double distance)
{
    requireSketch(objectAt(objects_, profile));
    return append(Extrude{profile, distance});
}

ObjectId Document::addRevolve(ObjectId profile, geometry::Axis axis, double offset, double angle, ObjectId datumAxis)
{
    requireSketch(objectAt(objects_, profile));
    return append(Revolve{profile, axis, offset, angle, datumAxis});
}

ObjectId Document::addBoolean(ObjectId first, ObjectId second, geometry::BooleanOperation operation)
{
    if (first == second) {
        throw std::invalid_argument("Choose two different bodies.");
    }
    for (const auto id : {first, second}) {
        if (!isBody(objectAt(objects_, id).parameters)) {
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

ObjectId Document::addEdgeTreatment(ObjectId body, std::vector<geometry::EdgeReference> edges,
                                    geometry::EdgeTreatmentKind kind, double size)
{
    requireBody(objectAt(objects_, body));
    if (edges.empty() || edges.size() > 64) {
        throw std::invalid_argument("Select between one and 64 edges.");
    }
    return append(EdgeTreatment{body, edges.front(), kind, size, {edges.begin() + 1, edges.end()}});
}

ObjectId Document::addAdvanced(Parameters parameters)
{
    if (!isAdvanced(parameters)) {
        throw std::invalid_argument("Choose an advanced modeling operation.");
    }
    return append(std::move(parameters));
}

bool Document::editAdvanced(ObjectId id, Parameters parameters)
{
    const auto& before = objectAt(objects_, id).parameters;
    if (!isAdvanced(parameters) || before.index() != parameters.index()) {
        throw std::invalid_argument("Choose the matching advanced modeling operation.");
    }
    if (std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, geometry::DatumPlane> || std::is_same_v<T, geometry::DatumAxis>
            || std::is_same_v<T, Sweep> || std::is_same_v<T, Loft> || std::is_same_v<T, Shell>
            || std::is_same_v<T, DraftAngle>) {
            return value == std::get<T>(before);
        }
        return false;
    }, parameters)) {
        return false;
    }
    // Keep the operation's dependencies and visibility rules stable while editing it.
    const auto currentInputs = inputs(parameters);
    const auto oldInputs = inputs(before);
    if (!std::is_permutation(currentInputs.begin(), currentInputs.end(), oldInputs.begin(), oldInputs.end())) {
        throw std::invalid_argument("Edit the parameters of the original inputs.");
    }
    validateAdvancedInputs(objects_, parameters, id);
    auto next = objects_;
    next[id - 1].parameters = std::move(parameters);
    rebuild(next, id - 1);
    commit(std::move(next));
    return true;
}

bool Document::setDensity(ObjectId id, std::optional<double> density)
{
    const auto& object = objectAt(objects_, id);
    requireBody(object);
    if (density && (!std::isfinite(*density) || *density <= 0 || *density > 1.e9)) {
        throw std::invalid_argument("Enter a positive material density.");
    }
    if (object.densityKgM3 == density) {
        return false;
    }
    auto next = objects_;
    next[id - 1].densityKgM3 = density;
    commit(std::move(next));
    return true;
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

ObjectId Document::addLinkedComponent(LinkedComponent component)
{
    return append(std::move(component));
}

bool Document::refreshLinkedComponents(std::vector<std::pair<ObjectId, LinkedComponent>> components)
{
    auto next = objects_;
    std::size_t first = next.size();
    std::vector<std::size_t> changed;
    for (auto& [id, incoming] : components) {
        const auto* before = std::get_if<LinkedComponent>(&objectAt(objects_, id).parameters);
        if (!before) {
            throw std::invalid_argument("Select a linked component.");
        }
        if (before->sourcePath == incoming.sourcePath && before->sourceObject == incoming.sourceObject
            && before->digest == incoming.digest && before->record == incoming.record) {
            continue;
        }
        const auto index = static_cast<std::size_t>(id - 1);
        first = std::min(first, index);
        changed.push_back(index);
        next[index].parameters = std::move(incoming);
    }
    if (first == next.size()) {
        return false;
    }
    // Rebuild once so simultaneous updates are evaluated against all new snapshots.
    rebuild(next, first, true);
    for (const auto index : changed) {
        if (next[index].status != Status::Ready) {
            throw std::invalid_argument(next[index].error);
        }
    }
    commit(std::move(next));
    return true;
}

bool Document::setPartProperties(ObjectId id, PartProperties properties)
{
    const auto& object = objectAt(objects_, id);
    requireBody(object);
    if (std::ranges::any_of(std::array{&properties.designation, &properties.name, &properties.material, &properties.note},
        [](const auto* value) { return value->size() > 4000; })) {
        throw std::invalid_argument("Invalid component properties.");
    }
    if (object.properties == properties) {
        return false;
    }
    auto next = objects_;
    next[id - 1].properties = std::move(properties);
    commit(std::move(next));
    return true;
}

ComponentRecord Document::componentRecord(ObjectId id) const
{
    const auto& object = objectAt(objects_, id);
    ComponentRecord record;
    if (const auto* linked = std::get_if<LinkedComponent>(&object.parameters)) {
        record = linked->record;
    }
    for (auto field : {&PartProperties::name, &PartProperties::designation, &PartProperties::material, &PartProperties::note}) {
        if (!(object.properties.*field).empty()) {
            record.properties.*field = object.properties.*field;
        }
    }
    if (object.densityKgM3) {
        record.densityKgM3 = object.densityKgM3;
    }
    if (const auto* assembly = std::get_if<Assembly>(&object.parameters)) {
        for (const auto& component : assembly->components) {
            record.children.push_back(componentRecord(component.body));
        }
    }
    return record;
}

geometry::AssemblySolution Document::assemblySolution(const Assembly& assembly) const
{
    return geometry::solveAssembly(placedBodies(objects_, assembly), assembly.mates);
}

Document Document::fromDefinitions(std::vector<Definition> definitions)
{
    Document document;
    auto& records = document.objects_;
    records.reserve(definitions.size());
    for (auto& definition : definitions) {
        const auto id = static_cast<ObjectId>(records.size() + 1);
        const auto& parameters = definition.parameters;
        validateAdvancedInputs(records, parameters, id);
        if (definition.densityKgM3 && (!isBody(parameters) || !std::isfinite(*definition.densityKgM3)
            || *definition.densityKgM3 <= 0 || *definition.densityKgM3 > 1.e9)) {
            throw std::invalid_argument("Invalid saved material density.");
        }
        const auto requirePrior = [&records](ObjectId source) -> const Object& {
            return objectAt(records, source);
        };
        if (const auto* sketch = std::get_if<geometry::Sketch>(&parameters)) {
            geometry::validateSketchConstraints(*sketch);
            if (sketch->support) {
                requireBody(requirePrior(sketch->support->body));
            }
        } else if (const auto* extrusion = std::get_if<Extrude>(&parameters)) {
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
                || !isBody(requirePrior(boolean->first).parameters)
                || !isBody(requirePrior(boolean->second).parameters)) {
                throw std::invalid_argument("Invalid references in a boolean operation.");
            }
        }
        records.push_back({id, std::move(definition.parameters), {}, Status::Ready, {}, definition.visible, definition.densityKgM3, std::move(definition.properties)});
    }
    rebuild(records, 0, true);
    for (const auto& object : records) {
        if (object.status != Status::Ready
            && (std::holds_alternative<Primitive>(object.parameters)
                || (std::holds_alternative<geometry::Sketch>(object.parameters)
                    && !std::get<geometry::Sketch>(object.parameters).support && !std::get<geometry::Sketch>(object.parameters).datumPlane)
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
    validateAdvancedInputs(objects_, parameters, id);
    next.push_back({id, std::move(parameters), {}, Status::Ready, {}, true});
    rebuild(next, next.size() - 1);
    if (next.back().status != Status::Ready) {
        throw std::invalid_argument(next.back().error);
    }
    if (isBody(next.back().parameters)) {
        for (const auto input : inputs(next.back().parameters)) {
            if (!std::holds_alternative<geometry::DatumAxis>(next[input - 1].parameters)) {
                next[input - 1].visible = false;
            }
        }
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

bool Document::editRevolve(ObjectId id, geometry::Axis axis, double offset, double angle, std::optional<ObjectId> datumAxis)
{
    const auto* operation = std::get_if<Revolve>(&objectAt(objects_, id).parameters);
    if (!operation) {
        throw std::invalid_argument("Select a Revolve operation.");
    }
    const auto reference = datumAxis.value_or(operation->datumAxis);
    if (operation->axis == axis && operation->offset == offset && operation->angle == angle && reference == operation->datumAxis) {
        return false;
    }
    auto next = objects_;
    next[id - 1].parameters = Revolve{operation->profile, axis, offset, angle, reference};
    validateAdvancedInputs(objects_, next[id - 1].parameters, id);
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
    if (!existing) {
        throw std::invalid_argument("Select an assembly.");
    }
    if (assembly.components.size() < geometry::minAssemblyComponents
        || assembly.components.size() > geometry::maxAssemblyComponents) {
        throw std::invalid_argument("An assembly needs 2 to 100 components.");
    }
    for (const auto& component : assembly.components) {
        if (component.body >= id) {
            throw std::invalid_argument("An input must precede its operation.");
        }
        requireBody(objectAt(objects_, component.body));
    }
    if (existing->components == assembly.components && existing->mates == assembly.mates) {
        return false;
    }
    auto next = objects_;
    for (const auto& component : assembly.components) {
        if (std::ranges::none_of(existing->components, [&](const auto& prior) { return prior.body == component.body; })) {
            next[component.body - 1].visible = false;
        }
    }
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
