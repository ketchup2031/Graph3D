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
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pln.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Circ.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>

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
    MateMode mode;
    double value;
    double minimum;
    double maximum;
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
        if (relation.mode == MateMode::Angle || relation.mode == MateMode::Travel) { continue; }
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
    if (state.rotations == 1 && lockedAxis) { state.freeRotationAxis = coords(lockedAxis->XYZ()); }
    return -1;
}

int orientAngles(AssemblyComponentState& state, const std::vector<Resolved>& relations)
{
    const gp_Pnt pivot(state.transform.TranslationPart());
    std::vector<const Resolved*> angles;
    for (const auto& rel : relations) {
        if (rel.mode == MateMode::Angle) { angles.push_back(&rel); }
    }
    if (angles.empty()) { return -1; }
    const auto radians = [](double degrees) { return degrees * std::numbers::pi / 180; };
    if (state.rotations == 1) {
        const auto axis = vector(state.freeRotationAxis);
        for (const auto* rel : angles) {
            const auto from = vector(rel->source.direction).Transformed(state.transform);
            const auto to = targetDirection(*rel);
            const double want = radians(rel->value);
            if (std::abs(from.Angle(to) - want) <= angularTolerance) { continue; }
            const auto a = from - axis * from.Dot(axis);
            const auto b = to - axis * to.Dot(axis);
            const double norm = a.Magnitude() * b.Magnitude();
            if (norm <= angularTolerance) { return rel->index; }
            const double ratio = (std::cos(want) - from.Dot(axis)*to.Dot(axis)) / norm;
            if (std::abs(ratio) > 1 + angularTolerance) { return rel->index; }
            const double reference = std::atan2(axis.Dot(a.Crossed(b)), a.Dot(b));
            const double delta = std::acos(std::clamp(ratio, -1., 1.));
            const double a1 = std::remainder(reference - delta, 2*std::numbers::pi);
            const double a2 = std::remainder(reference + delta, 2*std::numbers::pi);
            rotate(state.transform, axis, std::abs(a1) < std::abs(a2) ? a1 : a2, pivot);
            break;
        }
    } else if (state.rotations == 3) {
        // Project onto the angular constraints iteratively in the current tangent space.
        // Use a finite correction at antiparallel normals where the gradient vanishes.
        for (int i = 0; i < 80; ++i) {
            bool converged = true;
            for (const auto* rel : angles) {
                const auto from = vector(rel->source.direction).Transformed(state.transform);
                const auto to = targetDirection(*rel);
                const double difference = from.Angle(to) - radians(rel->value);
                if (std::abs(difference) < angularTolerance / 4) { continue; }
                const auto cross = from.Crossed(to);
                rotate(state.transform, cross.Magnitude() > angularTolerance ? cross : crossBasis(from), difference, pivot);
                converged = false;
            }
            if (converged) { break; }
        }
    }
    Translation directions;
    for (const auto* rel : angles) {
        const auto from = vector(rel->source.direction).Transformed(state.transform);
        const auto to = targetDirection(*rel);
        if (std::abs(from.Angle(to) - radians(rel->value)) > angularTolerance) { return rel->index; }
        const auto cross = from.Crossed(to);
        if (cross.Magnitude() > angularTolerance) {
            if (state.rotations == 1) {
                if (std::abs(cross.Dot(vector(state.freeRotationAxis))) > angularTolerance) { directions.constrain(cross, 0); }
            } else { directions.constrain(cross.Normalized(), 0); }
        } else if (state.rotations == 3) {
            const auto across = crossBasis(from);
            directions.constrain(across, 0);
            directions.constrain(from.Crossed(across), 0);
        } else if (state.rotations == 1 && !parallel(from, vector(state.freeRotationAxis))) {
            directions.constrain(vector(state.freeRotationAxis), 0);
        }
    }
    state.rotations = std::max(0, state.rotations - static_cast<int>(directions.rows.size()));
    if (state.rotations != 1) { state.freeRotationAxis = {}; }
    return -1;
}

int translate(AssemblyComponentState& state, const std::vector<Resolved>& relations, bool fixed, double screwTravel)
{
    Translation translation;
    for (const auto& relation : relations) {
        if (relation.mode == MateMode::Angle || relation.mode == MateMode::Travel) { continue; }
        const auto target = targetDirection(relation);
        const gp_Vec delta(point(relation.source.point).Transformed(state.transform),
                           point(relation.target.point).Transformed(relation.targetTransform));
        const auto apply = [&](const gp_Vec& direction, double separation = 0) {
            const auto amount = direction.Dot(delta) + separation;
            return fixed ? std::abs(amount) <= linearTolerance : translation.constrain(direction, amount);
        };
        if (relation.source.kind == MateKind::Plane) {
            const auto reference = vector(relation.target.direction).Transformed(relation.targetTransform);
            if (!apply(reference, relation.mode == MateMode::Distance ? relation.value : 0)) {
                return relation.index;
            }
        } else {
            const auto across = crossBasis(target);
            if (!apply(across) || !apply(target.Crossed(across))) {
                return relation.index;
            }
            if (relation.mode == MateMode::Screw && !apply(target, screwTravel)) { return relation.index; }
        }
    }
    state.translations = fixed ? 0 : 3 - static_cast<int>(translation.rows.size());
    auto displacement = translation.delta();
    const auto separation = [&](const Resolved& relation, const gp_Vec& along) {
        return gp_Vec(point(relation.target.point).Transformed(relation.targetTransform),
            point(relation.source.point).Transformed(state.transform)).Dot(along) + displacement.Dot(along);
    };
    for (const auto& relation : relations) {
        if (relation.mode != MateMode::Travel || relation.maximum - relation.minimum > linearTolerance || fixed) { continue; }
        const auto along = vector(relation.target.direction).Transformed(relation.targetTransform);
        if (!translation.constrain(along, displacement.Dot(along) + relation.minimum - separation(relation, along))) {
            return relation.index;
        }
        displacement = translation.delta();
        state.translations = 3 - static_cast<int>(translation.rows.size());
    }
    for (int iteration = 0; iteration < 100; ++iteration) {
        bool changed = false;
        for (const auto& relation : relations) {
            if (relation.mode != MateMode::Travel) { continue; }
            const auto along = vector(relation.target.direction).Transformed(relation.targetTransform);
            const auto value = separation(relation, along);
            const auto clipped = std::clamp(value, relation.minimum, relation.maximum);
            if (std::abs(clipped - value) <= linearTolerance) { continue; }
            auto direction = along;
            for (const auto& equation : translation.rows) { direction -= equation.normal * direction.Dot(equation.normal); }
            if (fixed || direction.SquareMagnitude() <= angularTolerance) { return relation.index; }
            displacement += direction * ((clipped - value) / direction.SquareMagnitude());
            changed = true;
        }
        if (!changed) { break; }
    }
    for (const auto& relation : relations) {
        if (relation.mode != MateMode::Travel) { continue; }
        const auto value = separation(relation, vector(relation.target.direction).Transformed(relation.targetTransform));
        if (value < relation.minimum - linearTolerance || value > relation.maximum + linearTolerance) { return relation.index; }
    }
    state.transform.SetTranslationPart(gp_Vec(state.transform.TranslationPart()) + displacement);
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
static AssemblySolution solvePoses(std::span<const PlacedBody> bodies, std::span<const Mate> mates, double driveAngle)
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
    int screw = -1;
    if (!std::isfinite(driveAngle) || std::abs(driveAngle) > maxComponentOffset) {
        result.error = "The drive angle must be within ±1,000,000 degrees.";
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
        const bool range = std::isfinite(mate.minimum) && std::isfinite(mate.maximum)
            && mate.minimum <= mate.maximum && std::abs(mate.minimum) <= maxComponentOffset && std::abs(mate.maximum) <= maxComponentOffset;
        const bool finiteValue = std::isfinite(mate.value);
        const bool planes = mate.moving.kind == MateKind::Plane && mate.reference.kind == MateKind::Plane;
        const bool cylinders = mate.moving.kind == MateKind::Cylinder && mate.reference.kind == MateKind::Cylinder;
        const bool validMode = mate.mode == MateMode::Align
            || (mate.mode == MateMode::Distance && planes && finiteValue && std::abs(mate.value) <= maxComponentOffset)
            || (mate.mode == MateMode::Angle && finiteValue && mate.value >= 0 && mate.value <= 180)
            || (mate.mode == MateMode::Travel && planes && range)
            || (mate.mode == MateMode::Screw && cylinders && range && finiteValue && std::abs(mate.value) >= .001
                && std::abs(mate.value) <= maxComponentOffset);
        if (!valid(mate.moving) || !valid(mate.reference) || !validMode
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
        if (mate.mode == MateMode::Screw) {
            if (screw >= 0) {
                result.failedMate = i;
                result.error = "Use one screw drive per assembly.";
                return result;
            }
            screw = i;
            result.screwTravel = std::clamp(driveAngle * mate.value / 360, mate.minimum, mate.maximum);
            result.driveAngle = result.screwTravel * 360 / mate.value;
            if (std::abs(result.driveAngle) > maxComponentOffset) {
                result.failedMate = i;
                result.error = "The screw travel range is outside the available drive angle.";
                return result;
            }
        }
        resolved[mate.component - 1].push_back({i, *moving, *base, {}, mate.opposite,
            mate.mode, mate.value, mate.minimum, mate.maximum});
    }
    if (screw < 0 && driveAngle != 0) {
        result.error = "Add a screw drive before setting its rotation.";
        return result;
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
            result.failedMate = orientAngles(state, relations);
        }
        if (result.failedMate < 0 && screw >= 0 && mates[screw].target == i + 1) {
            const auto& cylinder = resolved[mates[screw].component - 1];
            const auto it = std::ranges::find(cylinder, screw, &Resolved::index);
            const auto axis = vector(it->target.direction).Transformed(state.transform);
            if (state.rotations != 1 || !parallel(axis, vector(state.freeRotationAxis))) {
                result.failedMate = screw;
                result.error = "The screw driver needs one free rotation about its cylindrical axis.";
                return result;
            }
            rotate(state.transform, axis, std::remainder(result.driveAngle, 360.) * std::numbers::pi / 180,
                point(it->target.point).Transformed(state.transform));
            state.rotations = 0;
            state.freeRotationAxis = {};
        }
        if (result.failedMate < 0) {
            result.failedMate = translate(state, relations, component.fixed, result.screwTravel);
        }
        if (result.failedMate >= 0) {
            result.error = "The mate conflicts with the component pose or another mate.";
            return result;
        }
        result.components.push_back(state);
    }
    return result;
}

AssemblySolution solveAssembly(std::span<const PlacedBody> bodies, std::span<const Mate> mates, double driveAngle)
{
    auto result = solvePoses(bodies, mates, driveAngle);
    if (!result.error.empty() || std::ranges::none_of(bodies, [](const auto& body) { return body.context.has_value(); })) {
        return result;
    }
    std::vector<TopoDS_Shape> shapes;
    shapes.reserve(bodies.size());
    for (const auto& body : bodies) { shapes.push_back(body.shape); }
    std::vector<PlacedBody> updating;
    updating.reserve(bodies.size());
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        updating.push_back({shapes[i], bodies[i].placement, bodies[i].fixed});
    }
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        if (!bodies[i].context) { continue; }
        const auto& context = *bodies[i].context;
        const auto prefix = "Component #" + std::to_string(i + 1) + ": ";
        if (context.reference < 1 || static_cast<std::size_t>(context.reference) > i || !std::isfinite(context.clearance)
            || std::abs(context.clearance) > maxComponentOffset) {
            result.error = prefix + "Choose an earlier component and a finite contextual clearance.";
            return result;
        }
        const auto ref = static_cast<std::size_t>(context.reference - 1);
        try {
            resolveExactPlanarFace(shapes[i], context.face);
            const auto target = resolveSketchFace(shapes[ref], context.targetFace);
            const auto plane = BRepAdaptor_Surface(target).Plane();
            const auto pose = result.components[i].transform;
            const auto other = result.components[ref].transform;
            const auto along = vector(context.face.outward).Transformed(pose);
            if (!parallel(along, gp_Vec(plane.Axis().Direction()).Transformed(other))) {
                result.error = prefix + "The contextual planes are no longer parallel.";
                return result;
            }
            const auto amount = gp_Vec(point(context.face.center).Transformed(pose),
                plane.Location().Transformed(other)).Dot(along) + context.clearance;
            if (std::abs(amount) <= linearTolerance) { continue; }
            shapes[i] = movePlanarFace(shapes[i], context.face, amount);
            result = solvePoses(updating, mates, driveAngle);
            if (!result.error.empty()) { return result; }
            const auto changed = point(context.face.center).Translated(vector(context.face.outward) * amount)
                .Transformed(result.components[i].transform);
            const auto newNormal = vector(context.face.outward).Transformed(result.components[i].transform);
            if (!parallel(newNormal, gp_Vec(plane.Axis().Direction()).Transformed(other))
                || std::abs(gp_Vec(plane.Location().Transformed(other), changed).Dot(newNormal) - context.clearance) > linearTolerance) {
                result.error = prefix + "The contextual edit conflicts with the component mates.";
                return result;
            }
        } catch (const std::exception& error) {
            result.error = prefix + "Contextual reference or edit failed: " + error.what();
            return result;
        } catch (const Standard_Failure& error) {
            result.error = prefix + "Contextual reference or edit failed: "
                + (error.GetMessageString() ? std::string(error.GetMessageString()) : std::string("Open CASCADE failed."));
            return result;
        }
    }
    result.contextualShapes = std::move(shapes);
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
