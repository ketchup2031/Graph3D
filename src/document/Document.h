#pragma once

#include "geometry/Primitive.h"
#include "geometry/Sketch.h"

#include <TopoDS_Shape.hxx>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace graph3d::document {

using ObjectId = int;

struct Primitive { geometry::Primitive kind; double x; double y; };
struct Extrude { ObjectId profile; double distance; };
struct Revolve { ObjectId profile; geometry::Axis axis; double offset; double angle; };
struct Boolean { ObjectId first, second; geometry::BooleanOperation operation; };
using Parameters = std::variant<Primitive, geometry::Sketch, Extrude, Revolve, Boolean>;

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
    bool editExtrude(ObjectId id, double distance);
    bool editRevolve(ObjectId id, geometry::Axis axis, double offset, double angle);
    bool setVisible(ObjectId id, bool visible);
    bool clear();

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
    static void rebuild(State& state, std::size_t first);

    State objects_;
    std::vector<State> undo_;
    std::vector<State> redo_;
};

}  // namespace graph3d::document
