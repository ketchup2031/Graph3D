#pragma once

#include "document/Formula.h"

#include "geometry/AdvancedModeling.h"
#include "geometry/Assembly.h"
#include "geometry/BodyTransform.h"
#include "geometry/PartFeatures.h"
#include "geometry/Fastener.h"
#include "geometry/SheetMetal.h"
#include "geometry/SurfaceModeling.h"
#include "geometry/Primitive.h"
#include "geometry/SketchModeling.h"

#include <TopoDS_Shape.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <variant>
#include <vector>

namespace graph3d::document {

using ObjectId = int;

struct Primitive { geometry::Primitive kind; double x; double y; };
struct Extrude { ObjectId profile; double distance; };
struct Revolve { ObjectId profile; geometry::Axis axis; double offset; double angle; ObjectId datumAxis = 0; };
struct Boolean { ObjectId first, second; geometry::BooleanOperation operation; };
struct Imported { std::string name; TopoDS_Shape sourceShape; };
struct ImportedSurface { std::string name; TopoDS_Shape sourceShape; };
struct Surface {
    geometry::SurfaceKind kind;
    std::vector<ObjectId> sources;
    double distance = .001;
    int index = 1;
    bool operator==(const Surface&) const = default;
};
enum class AnnotationKind { Linear, Diameter, Datum, Roughness };
struct Annotation3d {
    AnnotationKind kind;
    ObjectId body;
    int first = 1, second = 2; // Vertex indices for a linear dimension.
    geometry::FaceReference face{};
    std::string text;
    std::size_t vertexCount = 0;
    bool operator==(const Annotation3d&) const = default;
};
struct ResolvedAnnotation {
    AnnotationKind kind;
    geometry::Point3 first, second, normal;
    double value = 0;
    std::string text;
};
struct MaterialEntry {
    const char* name;
    double densityKgM3;
};
std::span<const MaterialEntry> materialCatalog();

struct Measurement {
    geometry::MassProperties geometric;
    std::optional<double> massKg;
    std::optional<geometry::Point3> centerMm;
    // XX, YY, ZZ, XY, XZ, YZ about the centre of mass, in world axes and kg m².
    std::optional<std::array<double, 6>> inertiaKgM2;
    int missingDensities = 0;
};
struct Hole { ObjectId body; geometry::FaceReference face; geometry::HoleSize size; };
struct EdgeTreatment {
    ObjectId body;
    geometry::EdgeReference edge;
    geometry::EdgeTreatmentKind kind;
    double size;
    std::vector<geometry::EdgeReference> additionalEdges;
    bool followTangency = false;
    std::optional<double> endRadius;
    bool operator==(const EdgeTreatment&) const = default;
};
struct FeaturePattern {
    ObjectId seed;
    int count;
    geometry::Point3 step{};
    ObjectId path = 0;
    bool reverse = false;
    bool operator==(const FeaturePattern&) const = default;
};
struct MoveFace {
    ObjectId body;
    geometry::FaceReference face;
    double distance;
    bool operator==(const MoveFace&) const = default;
};
struct RemoveBlend {
    ObjectId body;
    geometry::BlendReference face;
    bool operator==(const RemoveBlend&) const = default;
};
struct Sweep {
    ObjectId profile;
    std::vector<geometry::Point3> path;
    ObjectId pathSketch = 0;
    bool reversePath = false;
    bool operator==(const Sweep&) const = default;
};
struct Loft { std::vector<ObjectId> profiles; bool ruled; bool operator==(const Loft&) const = default; };
struct Shell { ObjectId body; std::vector<geometry::FaceReference> openings; double thickness; bool operator==(const Shell&) const = default; };
struct DraftAngle {
    ObjectId body;
    geometry::FaceReference neutral;
    std::vector<geometry::FaceReference> walls;
    double degrees;
    bool operator==(const DraftAngle&) const = default;
};
struct Transform { ObjectId body; geometry::BodyTransform parameters; };
struct PartProperties {
    std::string designation;
    std::string name;
    std::string material;
    std::string note;
    std::optional<geometry::Point3> color;
    bool operator==(const PartProperties&) const = default;
};

struct ComponentRecord {
    PartProperties properties;
    std::optional<double> densityKgM3;
    std::vector<ComponentRecord> children;
    std::string identity;
    bool standard = false;
    bool operator==(const ComponentRecord&) const = default;
};

struct ExchangeNode {
    TopoDS_Shape shape;
    PartProperties properties;
    std::vector<ExchangeNode> children;
};
struct ImportedMesh {
    std::string name;
    std::vector<geometry::Point3> vertices; // Consecutive triangle triples in millimetres.
};

struct LinkedComponent {
    std::string sourcePath;
    ObjectId sourceObject;
    std::string digest;
    TopoDS_Shape snapshot;
    ComponentRecord record;
};

struct Component {
    ObjectId body;
    geometry::Placement placement;
    bool fixed = false;
    bool simplified = false;
    std::optional<geometry::ContextFaceEdit> context;
    bool operator==(const Component&) const = default;
};
struct Assembly {
    std::vector<Component> components;
    std::vector<geometry::Mate> mates;
    double driveAngle = 0;
};
struct MotionInterference {
    double requestedAngle;
    double solvedAngle;
    double travel;
    std::vector<geometry::Interference> pairs;
};
using Parameters = std::variant<Primitive, geometry::Sketch, Extrude, Revolve, Boolean, Imported,
                                Hole, EdgeTreatment, Transform, Assembly, geometry::DatumPlane, geometry::DatumAxis,
                                Sweep, Loft, Shell, DraftAngle, LinkedComponent, FeaturePattern, MoveFace, RemoveBlend,
                                geometry::Fastener, geometry::SheetMetal, ImportedMesh, geometry::SpatialCurve,
                                Surface, ImportedSurface, Annotation3d>;

bool isBody(const Parameters& parameters);
bool isSurface(const Parameters& parameters);
std::vector<geometry::Point3> annotationVertices(const TopoDS_Shape& shape);
bool isAdvanced(const Parameters& parameters);
std::vector<std::string> formulaFields(const Parameters& parameters);
ObjectId featureBase(const Parameters& parameters);

struct Definition {
    Parameters parameters;
    bool visible;
    std::optional<double> densityKgM3;
    PartProperties properties;
};

enum class Status { Ready, Failed, Blocked };

struct Object {
    ObjectId id;
    Parameters parameters;
    TopoDS_Shape shape;
    Status status = Status::Ready;
    std::string error;
    bool visible = true;
    std::optional<double> densityKgM3;
    PartProperties properties;
    TopoDS_Shape preview;
};

class Document {
public:
    ObjectId addPrimitive(geometry::Primitive kind);
    ObjectId addSketch(geometry::Sketch sketch);
    ObjectId addExtrude(ObjectId profile, double distance);
    ObjectId addRevolve(ObjectId profile, geometry::Axis axis, double offset, double angle, ObjectId datumAxis = 0);
    ObjectId addBoolean(ObjectId first, ObjectId second, geometry::BooleanOperation operation);
    ObjectId addImported(std::string name, TopoDS_Shape shape);
    ObjectId addImportedMesh(ImportedMesh mesh);
    ObjectId importTree(const std::vector<ExchangeNode>& roots);
    ObjectId addHole(ObjectId body, geometry::FaceReference face, geometry::HoleSize size);
    ObjectId addEdgeTreatment(ObjectId body, geometry::EdgeReference edge, geometry::EdgeTreatmentKind kind, double size);
    ObjectId addEdgeTreatment(ObjectId body, std::vector<geometry::EdgeReference> edges, geometry::EdgeTreatmentKind kind, double size);
    ObjectId addAdvanced(Parameters parameters);
    bool editAdvanced(ObjectId id, Parameters parameters);
    bool setDensity(ObjectId id, std::optional<double> densityKgM3);
    bool setCatalogMaterial(ObjectId id, std::string_view name);
    Measurement measure(ObjectId id, std::optional<double> densityOverride = {}) const;
    ObjectId addTransform(ObjectId body, geometry::BodyTransform parameters);
    ObjectId addAssembly(Assembly assembly);
    ObjectId addFastener(geometry::Fastener item);
    bool editFastener(ObjectId id, geometry::Fastener item);
    ObjectId addSheetMetal(geometry::SheetMetal sheet);
    bool editSheetMetal(ObjectId id, geometry::SheetMetal sheet);
    ObjectId addAnnotation(Annotation3d annotation);
    bool editAnnotation(ObjectId id, Annotation3d annotation);
    std::optional<ResolvedAnnotation> resolveAnnotation(ObjectId id) const;
    bool annotationBelongsTo(ObjectId annotation, ObjectId body) const;
    ObjectId addLinkedComponent(LinkedComponent component);
    bool refreshLinkedComponents(std::vector<std::pair<ObjectId, LinkedComponent>> components);
    bool setPartProperties(ObjectId id, PartProperties properties);
    ComponentRecord componentRecord(ObjectId id) const;
    geometry::AssemblySolution assemblySolution(const Assembly& assembly) const;
    std::vector<MotionInterference> scanAssembly(const Assembly& assembly, double start, double end, int samples) const;
    static Document fromDefinitions(std::vector<Definition> definitions, Configuration configuration = {});
    const Configuration& configuration() const { return configuration_; }
    bool setConfiguration(Configuration configuration);
    bool selectVariant(std::string name);
    Parameters effectiveParameters(ObjectId id) const;
    bool editSketch(ObjectId id, geometry::Sketch sketch);
    geometry::SketchContext sketchContext(const geometry::Sketch& sketch) const;
    bool editExtrude(ObjectId id, double distance);
    bool editRevolve(ObjectId id, geometry::Axis axis, double offset, double angle,
                     std::optional<ObjectId> datumAxis = {});
    bool editHole(ObjectId id, geometry::HoleSize size);
    bool editEdgeTreatment(ObjectId id, double size);
    bool editEdgeTreatment(ObjectId id, EdgeTreatment treatment);
    ObjectId addEdgeTreatment(EdgeTreatment treatment);
    bool editTransform(ObjectId id, geometry::BodyTransform parameters);
    bool editAssembly(ObjectId id, Assembly assembly);
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
    void rebuild(State& state, std::size_t first, bool all = false) const;
    static void rebuildConfigured(State&, std::size_t first, bool all, const Configuration&);

    struct Snapshot { State objects; std::uint64_t revision; Configuration configuration; };
    State objects_;
    Configuration configuration_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    std::uint64_t revision_ = 0;
    std::uint64_t nextRevision_ = 1;
};

}  // namespace graph3d::document
