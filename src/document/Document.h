#pragma once

#include "geometry/BodyTransform.h"
#include "geometry/PartFeatures.h"
#include "geometry/Primitive.h"
#include "geometry/Sketch.h"

#include <TopoDS_Shape.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace graph3d::document {

using ObjectId = int;

struct Primitive { geometry::Primitive kind; double x; double y; };
struct Extrude { ObjectId profile; double distance; };
struct Revolve { ObjectId profile; geometry::Axis axis; double offset; double angle; };
struct Boolean { ObjectId first, second; geometry::BooleanOperation operation; };
struct Imported { std::string name; TopoDS_Shape sourceShape; };
struct Hole { ObjectId body; geometry::FaceReference face; geometry::HoleSize size; };
struct EdgeTreatment {
    ObjectId body;
    geometry::EdgeReference edge;
    geometry::EdgeTreatmentKind kind;
    double size;
};
struct Transform { ObjectId body; geometry::BodyTransform parameters; };
using Parameters = std::variant<Primitive, geometry::Sketch, Extrude, Revolve, Boolean, Imported,
                                Hole, EdgeTreatment, Transform>;

struct Definition {
    Parameters parameters;
    bool visible;
};

enum class Status { Ready, Failed, Blocked };

struct Object {
    ObjectId id;
    Parameters parameters;
    TopoDS_Shape shape;
    Status status = Status::Ready;
    std::string error;
    bool visible = true;
};

class Document {
public:
    ObjectId addPrimitive(geometry::Primitive kind);
    ObjectId addSketch(geometry::Sketch sketch);
    ObjectId addExtrude(ObjectId profile, double distance);
    ObjectId addRevolve(ObjectId profile, geometry::Axis axis, double offset, double angle);
    ObjectId addBoolean(ObjectId first, ObjectId second, geometry::BooleanOperation operation);
    ObjectId addImported(std::string name, TopoDS_Shape shape);
    ObjectId addHole(ObjectId body, geometry::FaceReference face, geometry::HoleSize size);
    ObjectId addEdgeTreatment(ObjectId body, geometry::EdgeReference edge, geometry::EdgeTreatmentKind kind, double size);
    ObjectId addTransform(ObjectId body, geometry::BodyTransform parameters);
    static Document fromDefinitions(std::vector<Definition> definitions);
    bool editExtrude(ObjectId id, double distance);
    bool editRevolve(ObjectId id, geometry::Axis axis, double offset, double angle);
    bool editHole(ObjectId id, geometry::HoleSize size);
    bool editEdgeTreatment(ObjectId id, double size);
    bool editTransform(ObjectId id, geometry::BodyTransform parameters);
    bool setVisible(ObjectId id, bool visible);
    bool clear();

    std::uint64_t revision() const { return revision_; }
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool undo();
    bool redo();
    void clearHistory();
    const Object* find(ObjectId id) const;
    const std::vector<Object>& objects() const { return objects_; }

private:
    using State = std::vector<Object>;
    ObjectId append(Parameters parameters);
    void commit(State next);
    static void rebuild(State& state, std::size_t first, bool all = false);

    struct Snapshot { State objects; std::uint64_t revision; };
    State objects_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    std::uint64_t revision_ = 0;
    std::uint64_t nextRevision_ = 1;
};

}  // namespace graph3d::document
