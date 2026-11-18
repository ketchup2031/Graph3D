#include "document/Document.h"

#include <BRepGProp.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace graph3d::document {
namespace {

geometry::Point3 coordinates(const gp_Pnt& point)
{
    return {point.X(), point.Y(), point.Z()};
}

std::optional<ResolvedAnnotation> resolve(const Document& document, const Annotation3d& annotation)
{
    const auto* body = document.find(annotation.body);
    if (!body || body->status != Status::Ready || body->shape.IsNull() || !isBody(body->parameters)) { return {}; }
    ResolvedAnnotation result{annotation.kind, {}, {}, {0, 0, 1}, 0, annotation.text};
    switch (annotation.kind) {
    case AnnotationKind::Linear: {
        const auto vertices = annotationVertices(body->shape);
        if (annotation.first == annotation.second || annotation.first < 1 || annotation.second < 1
            || (annotation.vertexCount && annotation.vertexCount != vertices.size())
            || static_cast<std::size_t>(std::max(annotation.first, annotation.second)) > vertices.size()) { return {}; }
        result.first = vertices[annotation.first - 1];
        result.second = vertices[annotation.second - 1];
        const auto& a = result.first;
        const auto& b = result.second;
        result.value = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
        if (result.value < 1.e-7) { return {}; }
        break;
    }
    case AnnotationKind::Diameter: {
        const auto effective = document.effectiveParameters(annotation.body);
        const auto* hole = std::get_if<Hole>(&effective);
        if (!hole) { return {}; }
        result.first = result.second = hole->face.center;
        result.normal = hole->face.outward;
        result.value = hole->size.diameter;
        break;
    }
    case AnnotationKind::Datum:
    case AnnotationKind::Roughness: {
        if (result.text.empty()) { return {}; }
        const auto face = geometry::resolveSketchFace(body->shape, annotation.face);
        const auto plane = BRepAdaptor_Surface(face).Plane();
        const auto& xy = annotation.face.center;
        const gp_Pnt stored(xy[0], xy[1], xy[2]);
        const gp_Vec normal(plane.Axis().Direction());
        const auto dist = gp_Vec(plane.Location(), stored).Dot(normal);
        result.first = result.second = coordinates(stored.Translated(-dist * normal));
        result.normal = annotation.face.outward;
        break;
    }
    }
    return result;
}

}  // namespace

std::span<const MaterialEntry> materialCatalog()
{
    static constexpr std::array catalog{
        MaterialEntry{"Carbon steel", 7850}, MaterialEntry{"Stainless steel 304", 7900},
        MaterialEntry{"Aluminium 6061", 2700}, MaterialEntry{"Free-machining brass", 8500},
        MaterialEntry{"ABS plastic", 1040}
    };
    return catalog;
}

bool Document::setCatalogMaterial(ObjectId id, std::string_view name)
{
    const auto catalog = materialCatalog();
    const auto choice = std::ranges::find_if(catalog, [&](const auto& entry) { return entry.name == name; });
    const auto* object = find(id);
    if (!object || !isBody(object->parameters) || choice == catalog.end()) {
        throw std::invalid_argument("Select a solid and a material from the catalog.");
    }
    if (object->properties.material == name && object->densityKgM3 == choice->densityKgM3) { return false; }
    auto next = objects_;
    next[id - 1].properties.material = choice->name;
    next[id - 1].densityKgM3 = choice->densityKgM3;
    commit(std::move(next));
    return true;
}

Measurement Document::measure(ObjectId id, std::optional<double> densityOverride) const
{
    const auto* object = find(id);
    if (!object || object->status != Status::Ready || !isBody(object->parameters)) {
        throw std::invalid_argument("Select a valid solid or assembly.");
    }
    if (densityOverride && (!std::isfinite(*densityOverride) || *densityOverride < 0 || *densityOverride > 1.e9)) {
        throw std::invalid_argument("Enter a positive material density.");
    }
    Measurement result{geometry::massProperties(object->shape, 0)};
    auto record = componentRecord(id);
    if (densityOverride) { record.densityKgM3 = *densityOverride; }
    GProp_GProps weighted;
    const auto add = [&](auto&& self, const TopoDS_Shape& shape, const ComponentRecord& component, double fallback) -> void {
        const auto density = component.densityKgM3.value_or(fallback);
        if (component.children.empty()) {
            if (density <= 0) { ++result.missingDensities; return; }
            GProp_GProps properties;
            BRepGProp::VolumeProperties(shape, properties, true);
            if (properties.Mass() <= 0) { throw std::invalid_argument("The component has no measurable volume."); }
            weighted.Add(properties, density);
            return;
        }
        TopoDS_Iterator child(shape);
        for (const auto& part : component.children) {
            if (!child.More()) { throw std::invalid_argument("Assembly geometry and material records do not match."); }
            self(self, child.Value(), part, density);
            child.Next();
        }
        if (child.More()) { throw std::invalid_argument("Assembly geometry and material records do not match."); }
    };
    add(add, object->shape, record, 0);
    if (result.missingDensities == 0) {
        result.massKg = weighted.Mass() / 1.e9;
        result.centerMm = coordinates(weighted.CentreOfMass());
        const auto inertia = weighted.MatrixOfInertia();
        result.inertiaKgM2 = { {inertia(1,1) / 1.e15, inertia(2,2) / 1.e15, inertia(3,3) / 1.e15,
                               inertia(1,2) / 1.e15, inertia(1,3) / 1.e15, inertia(2,3) / 1.e15} };
    }
    return result;
}

std::vector<geometry::Point3> annotationVertices(const TopoDS_Shape& shape)
{
    TopTools_IndexedMapOfShape indexed;
    TopExp::MapShapes(shape, TopAbs_VERTEX, indexed);
    std::vector<geometry::Point3> result;
    result.reserve(indexed.Extent());
    for (int i = 1; i <= indexed.Extent(); ++i) {
        result.push_back(coordinates(BRep_Tool::Pnt(TopoDS::Vertex(indexed(i)))));
    }
    return result;
}

ObjectId Document::addAnnotation(Annotation3d annotation)
{
    if (!resolve(*this, annotation)) { throw std::invalid_argument("Select valid geometry for the 3D annotation."); }
    if (annotation.kind == AnnotationKind::Linear) {
        annotation.vertexCount = annotationVertices(find(annotation.body)->shape).size();
    }
    return append(std::move(annotation));
}

bool Document::editAnnotation(ObjectId id, Annotation3d annotation)
{
    const auto* object = find(id);
    const auto* previous = object ? std::get_if<Annotation3d>(&object->parameters) : nullptr;
    if (const auto* body = find(annotation.body); annotation.kind == AnnotationKind::Linear && body && !body->shape.IsNull()) {
        annotation.vertexCount = annotationVertices(body->shape).size();
    }
    if (!previous || annotation.kind != previous->kind || annotation.body != previous->body
        || annotation.text.size() > 160 || !resolve(*this, annotation)) {
        throw std::invalid_argument("Select valid geometry for the 3D annotation.");
    }
    if (*previous == annotation) { return false; }
    auto next = objects_;
    next[id - 1].parameters = std::move(annotation);
    commit(std::move(next));
    return true;
}

std::optional<ResolvedAnnotation> Document::resolveAnnotation(ObjectId id) const
{
    const auto* object = find(id);
    const auto* annotation = object ? std::get_if<Annotation3d>(&object->parameters) : nullptr;
    try {
        return annotation && object->status == Status::Ready ? resolve(*this, *annotation) : std::nullopt;
    } catch (const Standard_Failure&) {
        return {};
    } catch (const std::invalid_argument&) {
        return {};
    }
}

bool Document::annotationBelongsTo(ObjectId id, ObjectId body) const
{
    const auto* object = find(id);
    const auto* annotation = object ? std::get_if<Annotation3d>(&object->parameters) : nullptr;
    while (annotation && body >= annotation->body) {
        if (body == annotation->body) { return true; }
        const auto* candidate = find(body);
        if (!candidate) { break; }
        const auto* pattern = std::get_if<FeaturePattern>(&candidate->parameters);
        const auto parent = pattern ? pattern->seed : featureBase(candidate->parameters);
        if (parent >= body || std::holds_alternative<Transform>(candidate->parameters)) { break; }
        body = parent;
    }
    return false;
}

}  // namespace graph3d::document
