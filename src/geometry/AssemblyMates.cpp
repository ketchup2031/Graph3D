#include "geometry/Assembly.h"

#include <Adaptor3d_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>

#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pln.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Circ.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <cmath>

#include <optional>
#include <stdexcept>

namespace graph3d::geometry {
namespace {

constexpr double linearTolerance = 1.e-5;
constexpr double angularTolerance = 1.e-7;

gp_Pnt point(Point3 p) { return {p[0], p[1], p[2]}; }
gp_Vec vector(Point3 p) { return {p[0], p[1], p[2]}; }
Point3 coords(const gp_XYZ& p) { return {p.X(), p.Y(), p.Z()}; }

bool parallel(const gp_Vec& a, const gp_Vec& b)
{
    return a.Crossed(b).Magnitude() <= angularTolerance;
}

double axisDistance(const MateSurface& a, const MateSurface& b)
{
    const gp_Vec d(point(a.point), point(b.point));
    return d.Crossed(vector(a.direction)).Magnitude();
}

bool sameSupport(const MateSurface& a, const MateSurface& b)
{
    return a.kind == b.kind && parallel(vector(a.direction), vector(b.direction))
        && (a.kind == MateKind::Plane
            ? std::abs(gp_Vec(point(a.point), point(b.point)).Dot(vector(a.direction))) <= linearTolerance
              && vector(a.direction).Dot(vector(b.direction)) > 0
            : axisDistance(a, b) <= linearTolerance && std::abs(a.radius - b.radius) <= linearTolerance);
}

std::optional<MateSurface> match(const std::vector<MateSurface>& surfaces, const MateSurface& original)
{
    const auto exact = std::ranges::find_if(surfaces, [&](const auto& surface) { return sameSupport(surface, original); });
    if (exact != surfaces.end()) {
        return *exact;
    }
    // Accept a changed dimension only when a unique surface preserves the axis
    // of the bore or the normal line through the original planar centroid.
    std::optional<MateSurface> resized;
    for (const auto& surface : surfaces) {
        if (surface.kind != original.kind || !parallel(vector(surface.direction), vector(original.direction))
            || axisDistance(surface, original) > linearTolerance
            || (surface.kind == MateKind::Plane && vector(surface.direction).Dot(vector(original.direction)) <= 0)) {
            continue;
        }
        if (resized) {
            return {};
        }
        resized = surface;
    }
    return resized;
}

struct Equation {
    gp_Vec normal;
    double distance;
};

class Translation {
public:
    bool constrain(gp_Vec normal, double value)
    {
        for (const auto& equation : rows) {
            const double component = normal.Dot(equation.normal);
            normal -= component * equation.normal;
            value -= component * equation.distance;
        }
        const double norm = normal.Magnitude();
        if (norm <= angularTolerance) {
            return std::abs(value) <= linearTolerance;
        }
        rows.push_back({normal / norm, value / norm});
        return true;
    }

    gp_Vec delta() const
    {
        gp_Vec result;
        for (const auto& equation : rows) {
            result += equation.normal * equation.distance;
        }
        return result;
    }

    std::vector<Equation> rows;
};

gp_Vec crossBasis(const gp_Vec& along)
{
    return along.Crossed(std::abs(along.X()) < .8 ? gp_Vec(1, 0, 0) : gp_Vec(0, 1, 0)).Normalized();
}

void rotate(gp_Trsf& transform, const gp_Vec& axis, double angle, const gp_Pnt& pivot)
{
    if (std::abs(angle) > angularTolerance) {
        gp_Trsf correction;
        correction.SetRotation(gp_Ax1(pivot, gp_Dir(axis)), angle);
        transform.PreMultiply(correction);
    }
}

struct Resolved {
    int index;
    MateSurface source;
    MateSurface target;
    gp_Trsf targetTransform;
    bool opposite;
};

gp_Vec targetDirection(const Resolved& relation)
{
    const auto direction = vector(relation.target.direction).Transformed(relation.targetTransform);
    return relation.opposite ? -direction : direction;
}

int orient(AssemblyComponentState& state, const std::vector<Resolved>& relations, bool fixed)
{
    std::optional<gp_Vec> lockedAxis;
    const gp_Pnt pivot(state.transform.TranslationPart());
    for (const auto& relation : relations) {
        const auto source = vector(relation.source.direction).Transformed(state.transform);
        const auto target = targetDirection(relation);
        if (!fixed && state.rotations > 0) {
            if (!lockedAxis) {
                const auto cross = source.Crossed(target);
                const auto axis = cross.Magnitude() > angularTolerance ? cross : crossBasis(source);
                rotate(state.transform, axis, source.Angle(target), pivot);
                lockedAxis = target;
                state.rotations = 1;
            } else {
                const auto along = *lockedAxis;
                const auto from = source - along * source.Dot(along);
                const auto to = target - along * target.Dot(along);
                if (from.Magnitude() > angularTolerance && to.Magnitude() > angularTolerance) {
                    rotate(state.transform, along,
                        std::atan2(along.Dot(from.Crossed(to)), from.Dot(to)), pivot);
                    state.rotations = 0;
                }
            }
        }
        if ((vector(relation.source.direction).Transformed(state.transform) - target).Magnitude() > angularTolerance) {
            return relation.index;
        }
    }
    return -1;
}

int translate(AssemblyComponentState& state, const std::vector<Resolved>& relations, bool fixed)
{
    Translation translation;
    for (const auto& relation : relations) {
        const auto target = targetDirection(relation);
        const gp_Vec delta(point(relation.source.point).Transformed(state.transform),
                           point(relation.target.point).Transformed(relation.targetTransform));
        const auto apply = [&](const gp_Vec& direction) {
            const auto amount = direction.Dot(delta);
            return fixed ? std::abs(amount) <= linearTolerance : translation.constrain(direction, amount);
        };
        if (relation.source.kind == MateKind::Plane) {
            if (!apply(target)) {
                return relation.index;
            }
        } else {
            const auto across = crossBasis(target);
            if (!apply(across) || !apply(target.Crossed(across))) {
                return relation.index;
            }
        }
    }
    state.translations = fixed ? 0 : 3 - static_cast<int>(translation.rows.size());
    state.transform.SetTranslationPart(gp_Vec(state.transform.TranslationPart()) + translation.delta());
    return -1;
}

gp_Dir outsideDirection(const TopoDS_Face& face, const BRepAdaptor_Surface& surface,
                        const TopoDS_Shape& solid, gp_Dir direction)
{
    if (solid.IsNull()) {
        return direction;
    }
    BRepClass3d_SolidClassifier classifier(solid);
    for (double u : {.5, .25, .75, .1, .9}) {
        for (double v : {.5, .25, .75, .1, .9}) {
            const auto at = surface.Value(std::lerp(surface.FirstUParameter(), surface.LastUParameter(), u),
                                          std::lerp(surface.FirstVParameter(), surface.LastVParameter(), v));
            if (BRepClass_FaceClassifier(face, at, linearTolerance).State() != TopAbs_IN) {
                continue;
            }
            const gp_Vec step = gp_Vec(direction) * 1.e-4;
            classifier.Perform(at.Translated(step), 1.e-7);
            const auto front = classifier.State();
            classifier.Perform(at.Translated(-step), 1.e-7);
            const auto back = classifier.State();
            if (front == TopAbs_IN && back == TopAbs_OUT) {
                return direction.Reversed();
            }
            if (front == TopAbs_OUT && back == TopAbs_IN) {
                return direction;
            }
        }
    }
    return direction;
}

std::optional<MateSurface> planarSurface(const TopoDS_Face& face, const BRepAdaptor_Surface& surface,
                                        const TopoDS_Shape& solid)
{
    gp_Dir normal;
    if (surface.GetType() == GeomAbs_Plane) {
        normal = surface.Plane().Axis().Direction();
    } else if (surface.GetType() == GeomAbs_SurfaceOfRevolution) {
        const auto curve = surface.BasisCurve();
        const auto axis = surface.AxeOfRevolution();
        if (curve->GetType() != GeomAbs_Line
            || std::abs(curve->Line().Direction().Dot(axis.Direction())) > angularTolerance) {
            return {};
        }
        gp_Pnt at;
        gp_Vec du, dv;
        surface.D1((surface.FirstUParameter() + surface.LastUParameter()) / 2,
                   (surface.FirstVParameter() + surface.LastVParameter()) / 2, at, du, dv);
        normal = gp_Dir(du.Crossed(dv));
    } else {
        return {};
    }
    if (face.Orientation() == TopAbs_REVERSED) {
        normal.Reverse();
    }
    normal = outsideDirection(face, surface, solid, normal);
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(face, properties);
    return MateSurface{MateKind::Plane, coords(properties.CentreOfMass().XYZ()), coords(normal.XYZ()), 0};
}

std::optional<MateSurface> cylindricalSurface(const BRepAdaptor_Surface& surface)
{
    gp_Ax1 axis;
    double radius = 0;
    if (surface.GetType() == GeomAbs_Cylinder) {
        const auto cylinder = surface.Cylinder();
        axis = cylinder.Axis();
        radius = cylinder.Radius();
    } else if (surface.GetType() == GeomAbs_SurfaceOfRevolution) {
        const auto curve = surface.BasisCurve();
        axis = surface.AxeOfRevolution();
        if (curve->GetType() != GeomAbs_Line
            || !curve->Line().Direction().IsParallel(axis.Direction(), angularTolerance)) {
            return {};
        }
        radius = gp_Lin(axis).Distance(curve->Line().Location());
    } else if (surface.GetType() == GeomAbs_SurfaceOfExtrusion) {
        const auto curve = surface.BasisCurve();
        if (curve->GetType() != GeomAbs_Circle) {
            return {};
        }
        const auto circle = curve->Circle();
        if (!circle.Axis().Direction().IsParallel(surface.Direction(), angularTolerance)) {
            return {};
        }
        axis = circle.Axis();
        radius = circle.Radius();
    } else {
        return {};
    }
    if (radius <= linearTolerance) {
        return {};
    }
    const auto d = axis.Direction();
    const double leading = std::abs(d.X()) > angularTolerance ? d.X()
        : std::abs(d.Y()) > angularTolerance ? d.Y() : d.Z();
    if (leading < 0) {
        axis.Reverse();
    }
    const gp_Vec along(axis.Direction());
    const gp_Vec at(axis.Location().XYZ());
    return MateSurface{MateKind::Cylinder, coords((at - along * along.Dot(at)).XYZ()), coords(along.XYZ()), radius};
}

struct CollisionSolid {
    TopoDS_Shape shape;
    Bnd_Box bounds;
    std::vector<int> path;
};

void collectSolids(const TopoDS_Shape& shape, std::vector<int>& path, std::vector<CollisionSolid>& result)
{
    if (shape.ShapeType() == TopAbs_SOLID) {
        Bnd_Box bounds;
        BRepBndLib::AddOptimal(shape, bounds, false, false);
        result.push_back({shape, bounds, path});
        if (result.size() > 1000) {
            throw std::invalid_argument("An assembly can contain at most 1000 solids.");
        }
        return;
    }
    if (path.size() > 32) {
        throw std::invalid_argument("Assembly nesting is too deep.");
    }
    path.push_back(0);
    for (TopoDS_Iterator it(shape); it.More(); it.Next()) {
        ++path.back();
        collectSolids(it.Value(), path, result);
    }
    path.pop_back();
}

}  // namespace

std::vector<MateSurface> assemblySurfaces(const TopoDS_Shape& shape)
{
    TopTools_IndexedDataMapOfShapeListOfShape faces;
    if (!shape.IsNull()) {
        // The viewport can attach triangulations to shared shapes. Use the
        // analytic representation for point-in-solid tests and face matching.
        const auto analytic = BRepBuilderAPI_Copy(shape, true, false).Shape();
        TopExp::MapShapesAndUniqueAncestors(analytic, TopAbs_FACE, TopAbs_SOLID, faces);
    }
    std::vector<MateSurface> result;
    for (int index = 1; index <= faces.Extent(); ++index) {
        const auto face = TopoDS::Face(faces.FindKey(index));
        const auto& owners = faces.FindFromIndex(index);
        const BRepAdaptor_Surface surface(face);
        auto entry = planarSurface(face, surface, owners.IsEmpty() ? TopoDS_Shape{} : owners.First());
        if (!entry) {
            entry = cylindricalSurface(surface);
        }
        if (entry && std::ranges::none_of(result, [&](const auto& other) { return sameSupport(*entry, other); })) {
            result.push_back(*entry);
        }
    }
    return result;
}

// Components are solved against earlier instances. This avoids hidden grounding
// and lets the user's placement choose the pose in the degrees left free.
AssemblySolution solveAssembly(std::span<const PlacedBody> bodies, std::span<const Mate> mates)
{
    AssemblySolution result;
    result.components.reserve(bodies.size());
    std::vector<std::vector<Resolved>> resolved(bodies.size());
    std::vector<std::optional<std::vector<MateSurface>>> catalog(bodies.size());
    const auto surfaces = [&](int index) -> const auto& {
        auto& items = catalog[index];
        if (!items) {
            items = assemblySurfaces(bodies[index].shape);
        }
        return *items;
    };
    if (mates.size() > 512) {
        result.error = "An assembly can contain at most 512 mates.";
        return result;
    }
    for (int i = 0; i < static_cast<int>(mates.size()); ++i) {
        const auto& mate = mates[i];
        const auto valid = [](const MateSurface& surface) {
            return std::ranges::all_of(surface.point, [](double v) { return std::isfinite(v); })
                && std::abs(vector(surface.direction).Magnitude() - 1) <= angularTolerance
                && std::isfinite(surface.radius)
                && (surface.kind == MateKind::Plane ? surface.radius == 0 : surface.radius > 0);
        };
        if (!valid(mate.moving) || !valid(mate.reference)
            || mate.target < 1 || mate.component <= mate.target || mate.component > static_cast<int>(bodies.size())
            || mate.moving.kind != mate.reference.kind) {
            result.failedMate = i;
            result.error = "Choose two matching surfaces and an earlier reference component.";
            return result;
        }
        const auto moving = match(surfaces(mate.component - 1), mate.moving);
        const auto base = match(surfaces(mate.target - 1), mate.reference);
        if (!moving || !base) {
            result.failedMate = i;
            result.error = "A mated surface is missing or ambiguous after updating the component.";
            return result;
        }
        resolved[mate.component - 1].push_back({i, *moving, *base, {}, mate.opposite});
    }
    for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
        const auto& component = bodies[i];
        const int anchor = component.placement.anchor;
        if (anchor < 0 || anchor > i) {
            result.error = "An anchor must be the origin or an earlier component.";
            return result;
        }
        auto transform = assemblyPlacement(component.placement);
        if (anchor) {
            transform.PreMultiply(result.components[anchor - 1].transform);
        }
        AssemblyComponentState state{transform, component.fixed ? 0 : 3, component.fixed ? 0 : 3};
        auto& relations = resolved[i];
        for (auto& relation : relations) {
            relation.targetTransform = result.components[mates[relation.index].target - 1].transform;
        }
        result.failedMate = orient(state, relations, component.fixed);
        if (result.failedMate < 0) {
            result.failedMate = translate(state, relations, component.fixed);
        }
        if (result.failedMate >= 0) {
            result.error = "The mate conflicts with the component pose or another mate.";
            return result;
        }
        result.components.push_back(state);
    }
    return result;
}

std::vector<Interference> checkInterference(const TopoDS_Shape& assembly)
{
    if (assembly.IsNull()) {
        throw std::invalid_argument("Select a rebuilt assembly.");
    }
    std::vector<CollisionSolid> solids;
    std::vector<int> path;
    collectSolids(assembly, path, solids);
    std::vector<Interference> result;
    for (std::size_t i = 0; i < solids.size(); ++i) {
        for (std::size_t j = i + 1; j < solids.size(); ++j) {
            const auto& a = solids[i];
            const auto& b = solids[j];
            if (a.bounds.IsOut(b.bounds)) {
                continue;
            }
            BRepAlgoAPI_Common common(a.shape, b.shape);
            if (!common.IsDone() || common.HasErrors()) {
                throw std::runtime_error("Could not check the intersection of two components.");
            }
            if (common.Shape().IsNull()) {
                continue;
            }
            GProp_GProps volume;
            BRepGProp::VolumeProperties(common.Shape(), volume, true);
            if (volume.Mass() > 1.e-6) {
                result.push_back({a.path, b.path, volume.Mass()});
            }
        }
    }
    return result;
}

}  // namespace graph3d::geometry
