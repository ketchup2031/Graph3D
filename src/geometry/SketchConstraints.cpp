#include "geometry/SketchConstraints.h"
#include "geometry/SketchCurves.h"

#include <GeomAbs_CurveType.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <limits>
#include <ranges>
#include <type_traits>
#include <numbers>
#include <numeric>
#include <stdexcept>

namespace graph3d::geometry {
namespace {

constexpr double tolerance = 1.e-7;
constexpr double edgeTolerance = 1.e-5;
constexpr int maxConstraints = 2000;
constexpr double coordinateLimit = 1.e8;

Point2 difference(Point2 a, Point2 b) { return {a.u - b.u, a.v - b.v}; }
double length(Point2 p) { return std::hypot(p.u, p.v); }

using Values = std::vector<double>;
using Equation = std::function<double(const Values&)>;

struct Row {
    int constraint;
    std::vector<int> variables;
    Equation error;
};

double dot(const Values& a, const Values& b)
{
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0);
}

double derivative(const Row& row, Values& values, int column)
{
    auto& value = values[column];
    const auto original = value;
    const auto delta = std::max(1.e-5, 1.e-8 * std::abs(original));
    value = original + delta;
    const auto upper = row.error(values);
    value = original - delta;
    const auto lower = row.error(values);
    value = original;
    return (upper - lower) / (2 * delta);
}

void solveEquations(const std::vector<Row>& rows, Values& values)
{
    // Solve the linearized constraints together. The damping chooses a small
    // change when a sketch has free coordinates and regularizes redundant rows.
    constexpr double damping = 1.e-8;
    std::vector<Values> derivatives;
    derivatives.reserve(rows.size());
    for (const auto& row : rows) {
        derivatives.emplace_back(row.variables.size());
    }
    Values right(values.size()), diagonal(values.size()), direction(values.size()),
        step(values.size()), adjusted(values.size()), product(values.size());
    for (int iteration = 0; iteration < 48; ++iteration) {
        double score = 0, worst = 0;
        for (const auto& row : rows) {
            const double error = row.error(values);
            if (!std::isfinite(error)) {
                return;
            }
            score += error * error;
            worst = std::max(worst, std::abs(error));
        }
        if (worst <= tolerance / 8) {
            return;
        }
        std::ranges::fill(right, 0.0);
        std::ranges::fill(diagonal, damping);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            const double error = row.error(values);
            for (std::size_t k = 0; k < row.variables.size(); ++k) {
                const int index = row.variables[k];
                const auto partial = derivative(row, values, index);
                derivatives[i][k] = partial;
                right[index] -= partial * error;
                diagonal[index] += partial * partial;
            }
        }

        // Preconditioned conjugate gradients for (JᵀJ + λI) Δ = -Jᵀr.
        // Sparse rows keep the work linear in the number of constraints per iteration.
        std::ranges::fill(step, 0.0);
        for (std::size_t j = 0; j < values.size(); ++j) {
            direction[j] = adjusted[j] = right[j] / diagonal[j];
        }
        double energy = dot(right, adjusted);
        const auto precision = std::max(1.e-25, energy * 1.e-16);
        const int limit = std::min(256, static_cast<int>(values.size()) + 8);
        for (int linear = 0; linear < limit && energy > precision; ++linear) {
            std::ranges::transform(direction, product.begin(), [](double value) { return damping * value; });
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const auto& row = rows[i];
                double component = 0;
                for (std::size_t k = 0; k < row.variables.size(); ++k) {
                    component += derivatives[i][k] * direction[row.variables[k]];
                }
                for (std::size_t k = 0; k < row.variables.size(); ++k) {
                    product[row.variables[k]] += derivatives[i][k] * component;
                }
            }
            const double curvature = dot(direction, product);
            if (!std::isfinite(curvature) || curvature <= 0) {
                return;
            }
            const auto scale = energy / curvature;
            for (std::size_t j = 0; j < values.size(); ++j) {
                step[j] += scale * direction[j];
                right[j] -= scale * product[j];
                adjusted[j] = right[j] / diagonal[j];
            }
            const double next = dot(right, adjusted);
            const double ratio = next / energy;
            for (std::size_t j = 0; j < values.size(); ++j) {
                direction[j] = adjusted[j] + ratio * direction[j];
            }
            energy = next;
        }

        bool improved = false;
        for (double scale = 1; scale >= 1.0 / 256; scale /= 2) {
            for (std::size_t j = 0; j < values.size(); ++j) {
                product[j] = values[j] + scale * step[j];
            }
            double trial = 0;
            for (const auto& row : rows) {
                const double error = row.error(product);
                trial += error * error;
            }
            if (trial < score) {
                values.swap(product);
                improved = true;
                break;
            }
        }
        if (!improved) {
            return;
        }
    }
}

double angle(Point2 first, Point2 second)
{
    return std::atan2(first.u * second.v - first.v * second.u, first.u * second.u + first.v * second.v);
}

double distance(Point3 a, Point3 b)
{
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

// Return the orientation of a unique matching edge. Straight edges may lengthen
// along the same line when the support is edited.
int matchEdge(const EdgeReference& a, const EdgeReference& b, bool allowResize)
{
    if (a.curveType != b.curveType) {
        return 0;
    }
    bool forward = true, reverse = true;
    for (std::size_t i = 0; i < a.samples.size(); ++i) {
        forward &= distance(a.samples[i], b.samples[i]) <= edgeTolerance;
        reverse &= distance(a.samples[i], b.samples[b.samples.size() - 1 - i]) <= edgeTolerance;
    }
    if (forward || reverse) {
        return forward ? 1 : -1;
    }
    if (!allowResize || a.curveType != GeomAbs_Line) {
        return 0;
    }
    const auto& origin = a.samples.front();
    const double width = distance(origin, a.samples.back());
    if (width <= edgeTolerance) {
        return 0;
    }
    Point3 axis{};
    for (int i = 0; i < 3; ++i) {
        axis[i] = (a.samples.back()[i] - origin[i]) / width;
    }
    const auto coordinate = [&](Point3 point) {
        double t = 0;
        for (int i = 0; i < 3; ++i) {
            t += (point[i] - origin[i]) * axis[i];
        }
        Point3 onLine{};
        for (int i = 0; i < 3; ++i) {
            onLine[i] = origin[i] + t * axis[i];
        }
        if (distance(onLine, point) > edgeTolerance) {
            return std::optional<double>{};
        }
        return std::optional(t);
    };
    const auto start = coordinate(b.samples.front());
    const auto end = coordinate(b.samples.back());
    if (!start || !end || std::min(width, std::max(*start, *end)) - std::max(0.0, std::min(*start, *end)) <= edgeTolerance) {
        return 0;
    }
    return *start < *end ? 1 : -1;
}

Point2 projectionPoint(const SketchConstraint& relation, std::span<const SketchProjection> projections)
{
    for (bool allowResize : {false, true}) {
        std::optional<Point2> match;
        for (const auto& projection : projections) {
            const int orientation = matchEdge(*relation.edge, projection.edge, allowResize);
            if (!orientation || projection.points.size() < 2) {
                continue;
            }
            if (match) {
                throw std::invalid_argument("A projected sketch edge is ambiguous after rebuilding.");
            }
            const double position = orientation > 0 ? relation.value : 1 - relation.value;
            const double index = position * (projection.points.size() - 1);
            const auto i = std::min(static_cast<std::size_t>(index), projection.points.size() - 2);
            const auto a = projection.points[i];
            const auto b = projection.points[i + 1];
            match = Point2{std::lerp(a.u, b.u, index - i), std::lerp(a.v, b.v, index - i)};
        }
        if (match) {
            return *match;
        }
    }
    throw std::invalid_argument("A projected sketch edge could not be matched after rebuilding.");
}

}  // namespace

int sketchVertexCount(const SketchEntity& entity)
{
    return std::visit([](const auto& shape) -> int {
        using Type = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<Type, Line>) { return 2; }
        else if constexpr (std::is_same_v<Type, Rectangle>) { return 4; }
        else if constexpr (std::is_same_v<Type, Circle>) { return 1; }
        else if constexpr (std::is_same_v<Type, Arc>) { return 3; }
        else { return static_cast<int>(shape.points.size()); }
    }, entity);
}

int sketchSegmentCount(const SketchEntity& entity)
{
    return std::holds_alternative<Rectangle>(entity) ? 4 : std::holds_alternative<Line>(entity) ? 1 : 0;
}

std::vector<int> sketchEndpoints(const SketchEntity& entity)
{
    if (const auto* spline = std::get_if<Spline>(&entity)) {
        return spline->closed || spline->points.size() < 2
            ? std::vector<int>{} : std::vector{0, static_cast<int>(spline->points.size()) - 1};
    }
    if (std::holds_alternative<Arc>(entity)) { return {0, 2}; }
    if (std::holds_alternative<Line>(entity)) { return {0, 1}; }
    return {};
}

bool isConstruction(const Sketch& sketch, int entity)
{
    return std::ranges::binary_search(sketch.construction, entity);
}

bool usesSecondReference(ConstraintKind kind)
{
    using enum ConstraintKind;
    return kind == Coincident || kind == Perpendicular || kind == Tangent || kind == Parallel
        || kind == Equal || kind == Symmetric || kind == Angle;
}

Point2 sketchVertex(const SketchEntity& entity, int part)
{
    if (part < 0 || part >= sketchVertexCount(entity)) {
        throw std::invalid_argument("Invalid sketch point reference.");
    }
    return std::visit([part](const auto& shape) -> Point2 {
        using Type = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<Type, Line>) {
            return part == 0 ? shape.start : shape.end;
        } else if constexpr (std::is_same_v<Type, Rectangle>) {
            return {part == 1 || part == 2 ? shape.opposite.u : shape.first.u,
                    part == 2 || part == 3 ? shape.opposite.v : shape.first.v};
        } else if constexpr (std::is_same_v<Type, Circle>) {
            return shape.center;
        } else if constexpr (std::is_same_v<Type, Arc>) {
            return std::array{shape.start, shape.through, shape.end}[part];
        } else {
            return shape.points[part];
        }
    }, entity);
}

Line sketchSegment(const SketchEntity& entity, int part)
{
    if (part < 0 || part >= sketchSegmentCount(entity)) {
        throw std::invalid_argument("Invalid sketch segment reference.");
    }
    return {sketchVertex(entity, part), sketchVertex(entity, (part + 1) % sketchVertexCount(entity))};
}

namespace {

bool roundEntity(const SketchEntity& entity)
{
    return std::holds_alternative<Circle>(entity) || std::holds_alternative<Arc>(entity);
}

bool finitePoint(Point2 p)
{
    return std::isfinite(p.u) && std::isfinite(p.v)
        && std::abs(p.u) <= coordinateLimit && std::abs(p.v) <= coordinateLimit;
}

bool validGeometry(const SketchEntity& entity)
{
    const auto count = sketchVertexCount(entity);
    if (const auto* spline = std::get_if<Spline>(&entity)) {
        if (count < (spline->closed ? 3 : 2) || count > 128) {
            return false;
        }
        if (std::adjacent_find(spline->points.begin(), spline->points.end(), [](Point2 a, Point2 b) {
                return length(difference(a, b)) <= tolerance;
            }) != spline->points.end() || (spline->closed && length(difference(spline->points.front(), spline->points.back())) <= tolerance)) {
            return false;
        }
    }
    for (int part = 0; part < count; ++part) {
        if (!finitePoint(sketchVertex(entity, part))) {
            return false;
        }
    }
    if (const auto* line = std::get_if<Line>(&entity)) {
        return length(difference(line->end, line->start)) > tolerance;
    }
    if (const auto* box = std::get_if<Rectangle>(&entity)) {
        const auto delta = difference(box->opposite, box->first);
        return std::abs(delta.u) > tolerance && std::abs(delta.v) > tolerance;
    }
    if (const auto* circle = std::get_if<Circle>(&entity)) {
        return std::isfinite(circle->radius) && circle->radius > tolerance && circle->radius <= coordinateLimit;
    }
    if (const auto* arc = std::get_if<Arc>(&entity)) {
        return sketchArc(*arc).has_value();
    }
    return true;
}

struct VariableLayout {
    const Sketch& sketch;
    std::vector<int> starts;
    int count = 0;

    explicit VariableLayout(const Sketch& source) : sketch(source)
    {
        starts.reserve(sketch.entities.size() + 1);
        for (const auto& entity : sketch.entities) {
            starts.push_back(count);
            count += std::holds_alternative<Rectangle>(entity) ? 4 : std::holds_alternative<Circle>(entity) ? 3
                                                               : sketchVertexCount(entity) * 2;
        }
        starts.push_back(count);
    }

    Point2 vertex(const Values& x, SketchRef ref) const
    {
        const int base = starts[ref.entity];
        if (std::holds_alternative<Rectangle>(sketch.entities[ref.entity])) {
            return {x[base + (ref.part == 1 || ref.part == 2 ? 2 : 0)],
                    x[base + (ref.part == 2 || ref.part == 3 ? 3 : 1)]};
        }
        return {x[base + 2 * ref.part], x[base + 2 * ref.part + 1]};
    }

    Point2 vector(const Values& x, SketchRef ref) const
    {
        const auto start = vertex(x, ref);
        ref.part = (ref.part + 1) % sketchVertexCount(sketch.entities[ref.entity]);
        return difference(vertex(x, ref), start);
    }

    SketchEntity shape(const Values& x, int index) const
    {
        const auto get = [&](int part) { return vertex(x, {index, part}); };
        return std::visit([&](const auto& entity) -> SketchEntity {
            using Type = std::decay_t<decltype(entity)>;
            if constexpr (std::is_same_v<Type, Line>) { return Line{get(0), get(1)}; }
            else if constexpr (std::is_same_v<Type, Rectangle>) { return Rectangle{get(0), get(2)}; }
            else if constexpr (std::is_same_v<Type, Circle>) { return Circle{get(0), x[starts[index] + 2]}; }
            else if constexpr (std::is_same_v<Type, Arc>) { return Arc{get(0), get(1), get(2)}; }
            else {
                auto spline = entity;
                for (int i = 0; i < static_cast<int>(spline.points.size()); ++i) {
                    spline.points[i] = get(i);
                }
                return spline;
            }
        }, sketch.entities[index]);
    }

    std::optional<ArcGeometry> round(const Values& x, int index) const
    {
        if (std::holds_alternative<Circle>(sketch.entities[index])) {
            const auto radius = x[starts[index] + 2];
            return radius > 0 ? std::optional(ArcGeometry{vertex(x, {index, 0}), radius, 0, 2 * std::numbers::pi})
                              : std::nullopt;
        }
        return sketchArc({vertex(x, {index, 0}), vertex(x, {index, 1}), vertex(x, {index, 2})});
    }

    Values pack() const
    {
        Values x;
        x.reserve(count);
        const auto append = [&](Point2 p) { x.push_back(p.u); x.push_back(p.v); };
        for (const auto& entity : sketch.entities) {
            if (const auto* rectangle = std::get_if<Rectangle>(&entity)) {
                append(rectangle->first);
                append(rectangle->opposite);
            } else if (const auto* circle = std::get_if<Circle>(&entity)) {
                append(circle->center);
                x.push_back(circle->radius);
            } else {
                for (int part = 0; part < sketchVertexCount(entity); ++part) {
                    append(sketchVertex(entity, part));
                }
            }
        }
        return x;
    }
};

double cross(Point2 a, Point2 b) { return a.u * b.v - a.v * b.u; }
double dot(Point2 a, Point2 b) { return a.u * b.u + a.v * b.v; }
Point2 scaled(Point2 a, double s) { return {a.u * s, a.v * s}; }
Point2 plus(Point2 a, Point2 b) { return {a.u + b.u, a.v + b.v}; }

using SparseRow = std::map<int, double>;
constexpr double rankTolerance = 1.e-6;

void subtract(SparseRow& values, const SparseRow& pivot, double factor)
{
    for (const auto& [column, coefficient] : pivot) {
        auto& value = values[column];
        value -= coefficient * factor;
        if (std::abs(value) < rankTolerance) {
            values.erase(column);
        }
    }
}

void degreesOfFreedom(const std::vector<Row>& rows, Values& x, const VariableLayout& layout, SketchSolution& result)
{
    // Eliminate sparsely so unrelated geometry does not form a dense sketch-sized matrix.
    struct Pivot { int column; SparseRow rest; };
    std::vector<Pivot> pivots;
    std::vector<int> indexOf(x.size(), -1);
    for (const auto& equation : rows) {
        SparseRow row;
        double scale = 0;
        for (const auto column : equation.variables) {
            const double value = derivative(equation, x, column);
            if (std::isfinite(value)) {
                row[column] = value;
                scale = std::max(scale, std::abs(value));
            }
        }
        if (scale < rankTolerance) {
            continue;
        }
        for (auto& [column, value] : row) {
            value /= scale;
        }
        std::erase_if(row, [](const auto& entry) { return std::abs(entry.second) < rankTolerance; });
        for (;;) {
            // Pivot rows only contain later pivots. Eliminate in pivot order.
            auto next = row.end();
            for (auto i = row.begin(); i != row.end(); ++i) {
                const int p = indexOf[i->first];
                if (p >= 0 && (next == row.end() || p < indexOf[next->first])) {
                    next = i;
                }
            }
            if (next == row.end()) {
                break;
            }
            const auto [column, factor] = *next;
            row.erase(next);
            subtract(row, pivots[indexOf[column]].rest, factor);
        }
        if (!row.empty()) {
            const auto lead = std::ranges::max_element(row, {}, [](const auto& entry) { return std::abs(entry.second); });
            const auto [column, factor] = *lead;
            row.erase(lead);
            for (auto& [other, value] : row) {
                value /= factor;
            }
            indexOf[column] = static_cast<int>(pivots.size());
            pivots.push_back({column, std::move(row)});
        }
    }
    std::vector<bool> fixed(x.size());
    for (auto pivot = pivots.rbegin(); pivot != pivots.rend(); ++pivot) {
        auto& rest = pivot->rest;
        for (auto it = rest.begin(); it != rest.end();) {
            const auto [column, factor] = *it;
            if (indexOf[column] < 0) {
                ++it;
            } else {
                rest.erase(it);
                subtract(rest, pivots[indexOf[column]].rest, factor);
                it = rest.begin();
            }
        }
        fixed[pivot->column] = rest.empty();
    }
    result.degreesOfFreedom = static_cast<int>(x.size() - pivots.size());
    for (std::size_t i = 1; i < layout.starts.size(); ++i) {
        result.fullyDefined.push_back(std::all_of(fixed.begin() + layout.starts[i - 1], fixed.begin() + layout.starts[i],
                                                 [](bool value) { return value; }));
    }
}

}  // namespace

void validateSketchConstraints(const Sketch& sketch)
{
    using enum ConstraintKind;
    if (sketch.constraints.size() > maxConstraints) {
        throw std::invalid_argument("Too many sketch constraints.");
    }
    if (!std::ranges::is_sorted(sketch.construction)
        || std::adjacent_find(sketch.construction.begin(), sketch.construction.end()) != sketch.construction.end()
        || (!sketch.construction.empty() && (sketch.construction.front() < 0
            || static_cast<std::size_t>(sketch.construction.back()) >= sketch.entities.size()))) {
        throw std::invalid_argument("Invalid construction geometry references.");
    }
    if (!std::ranges::all_of(sketch.entities, validGeometry)) {
        throw std::invalid_argument("The sketch contains invalid or degenerate geometry.");
    }
    const auto entity = [&](SketchRef ref) -> const SketchEntity& {
        if (ref.entity < 0 || static_cast<std::size_t>(ref.entity) >= sketch.entities.size()) {
            throw std::invalid_argument("Invalid sketch entity reference.");
        }
        return sketch.entities[ref.entity];
    };
    const auto line = [&](SketchRef ref) { sketchSegment(entity(ref), ref.part); };
    const auto point = [&](SketchRef ref) { sketchVertex(entity(ref), ref.part); };
    const auto round = [&](SketchRef ref) { return roundEntity(entity(ref)) && ref.part == 0; };
    for (const auto& item : sketch.constraints) {
        const auto a = item.first;
        const auto b = item.second;
        if (!std::isfinite(item.value)) {
            throw std::invalid_argument("Enter a finite sketch dimension.");
        }
        if (item.kind != Coincident && usesSecondReference(item.kind) && a == b) {
            throw std::invalid_argument("Choose two different sketch references.");
        }
        switch (item.kind) {
        case Coincident:
        case Symmetric:
            point(a);
            point(b);
            if (item.kind == Symmetric) {
                line(item.axis);
            }
            break;
        case FixPoint:
            point(a);
            if (!finitePoint(item.fixed)) {
                throw std::invalid_argument("Enter a finite fixed point.");
            }
            break;
        case OnProjection:
            point(a);
            if (!sketch.support || !item.edge || item.value < 0 || item.value > 1) {
                throw std::invalid_argument("Invalid projected sketch edge reference.");
            }
            break;
        case Angle:
        case Parallel:
        case Perpendicular:
            line(a);
            line(b);
            if (item.kind == Angle && (!(item.value > 0) || item.value > 180)) {
                throw std::invalid_argument("Enter an angle greater than zero and at most 180°.");
            }
            break;
        case Horizontal:
        case Vertical:
        case Length:
            line(a);
            break;
        case Diameter:
            if (!round(a)) {
                throw std::invalid_argument("Select a circle or arc for a diameter constraint.");
            }
            break;
        case Equal:
            if (round(a) != round(b)) {
                throw std::invalid_argument("Use two straight edges or two circular curves for equality.");
            }
            if (!round(a)) {
                line(a);
                line(b);
            }
            break;
        case Tangent:
            if (!round(a) && !round(b)) {
                throw std::invalid_argument("Tangency requires a circular curve and a straight or circular edge.");
            }
            if (!round(a)) { line(a); }
            if (!round(b)) { line(b); }
            break;
        }
        if ((item.kind == Diameter || item.kind == Length) && (item.value <= tolerance || item.value > coordinateLimit)) {
            throw std::invalid_argument("Enter a positive sketch dimension.");
        }
    }
}

namespace {

constexpr double turn = 2 * std::numbers::pi;
constexpr double invalidEquation = std::numeric_limits<double>::quiet_NaN();

bool onArc(const ArcGeometry& arc, Point2 point)
{
    const auto radius = difference(point, arc.center);
    const auto theta = std::atan2(radius.v, radius.u);
    const auto relative = arc.sweep > 0 ? theta - arc.start : arc.start - theta;
    const auto normalized = std::fmod(std::fmod(relative, turn) + turn, turn);
    return normalized <= std::abs(arc.sweep) + 1.e-5 || normalized >= turn - 1.e-5;
}

struct Tangency {
    SketchRef first;
    SketchRef second;
    std::optional<SketchRef> contact;
    bool lines = false;
    bool internal = false;
    double side = 1;
    double scale = 1;
};

std::optional<SketchRef> sharedContact(const Sketch& sketch, SketchRef curve, std::span<const SketchRef> ends)
{
    for (int part : sketchEndpoints(sketch.entities[curve.entity])) {
        const SketchRef ref{curve.entity, part};
        const auto p = sketchVertex(sketch.entities[curve.entity], part);
        for (const auto end : ends) {
            const auto joinable = sketchEndpoints(sketch.entities[end.entity]);
            const bool implicit = !isConstruction(sketch, curve.entity) && !isConstruction(sketch, end.entity)
                && std::ranges::find(joinable, end.part) != joinable.end()
                && length(difference(p, sketchVertex(sketch.entities[end.entity], end.part))) <= tolerance;
            if (implicit || std::ranges::any_of(sketch.constraints, [&](const auto& item) {
                    return item.kind == ConstraintKind::Coincident
                        && ((item.first == ref && item.second == end) || (item.second == ref && item.first == end));
                })) {
                return ref;
            }
        }
    }
    return {};
}

Tangency describeTangency(const VariableLayout& layout, const Values& x, SketchRef a, SketchRef b)
{
    const auto& sketch = layout.sketch;
    if (roundEntity(sketch.entities[a.entity]) && !roundEntity(sketch.entities[b.entity])) {
        std::swap(a, b);
    }
    Tangency tangent{a, b, {}, !roundEntity(sketch.entities[a.entity])};
    const auto second = layout.round(x, b.entity).value();
    if (tangent.lines) {
        const auto line = layout.vector(x, a);
        const auto origin = layout.vertex(x, a);
        tangent.side = std::copysign(1.0, cross(line, difference(second.center, origin)));
        tangent.scale = std::max(second.radius, 1.0);
        const int end = (a.part + 1) % sketchVertexCount(sketch.entities[a.entity]);
        const std::array endpoints{a, SketchRef{a.entity, end}};
        tangent.contact = sharedContact(sketch, b, endpoints);
    } else {
        const auto first = layout.round(x, a.entity).value();
        const auto gap = length(difference(second.center, first.center));
        tangent.internal = std::abs(gap - std::abs(first.radius - second.radius))
            < std::abs(gap - first.radius - second.radius);
        tangent.side = std::copysign(1.0, first.radius - second.radius);
        std::vector<SketchRef> endpoints;
        for (int part : sketchEndpoints(sketch.entities[a.entity])) {
            endpoints.push_back({a.entity, part});
        }
        tangent.contact = sharedContact(sketch, b, endpoints);
        tangent.scale = std::max(std::min(first.radius, second.radius), 1.0);
    }
    return tangent;
}

double tangentError(const VariableLayout& layout, const Values& x, const Tangency& tangent)
{
    const auto b = layout.round(x, tangent.second.entity);
    if (!b) {
        return invalidEquation;
    }
    if (tangent.lines) {
        const auto v = layout.vector(x, tangent.first);
        if (length(v) <= tolerance) { return invalidEquation; }
        if (tangent.contact) {
            return dot(v, difference(layout.vertex(x, *tangent.contact), b->center)) / length(v);
        }
        return cross(v, difference(b->center, layout.vertex(x, tangent.first))) / length(v) - tangent.side * b->radius;
    }
    const auto a = layout.round(x, tangent.first.entity);
    if (!a) {
        return invalidEquation;
    }
    if (tangent.contact) {
        const auto point = layout.vertex(x, *tangent.contact);
        const double delta = angle(difference(point, a->center), difference(point, b->center));
        return std::remainder(delta - (tangent.internal ? 0 : std::numbers::pi), turn) * tangent.scale;
    }
    const auto target = tangent.internal ? tangent.side * (a->radius - b->radius) : a->radius + b->radius;
    return length(difference(b->center, a->center)) - target;
}

bool actualContact(const VariableLayout& layout, const Values& x, const Tangency& tangent)
{
    const auto b = layout.round(x, tangent.second.entity);
    if (!b) { return false; }
    if (tangent.lines) {
        const auto v = layout.vector(x, tangent.first);
        if (length(v) <= tolerance) { return false; }
        const auto origin = layout.vertex(x, tangent.first);
        const double t = dot(difference(b->center, origin), v) / dot(v, v);
        const auto contact = plus(origin, scaled(v, t));
        return t >= -1.e-6 && t <= 1 + 1.e-6
            && std::abs(length(difference(contact, b->center)) - b->radius) <= 1.e-5 && onArc(*b, contact);
    }
    const auto a = layout.round(x, tangent.first.entity);
    if (!a) { return false; }
    const auto v = difference(b->center, a->center);
    if (length(v) <= tolerance) { return false; }
    const auto contact = plus(a->center, scaled(v, a->radius / length(v) * (tangent.internal ? tangent.side : 1)));
    return std::abs(length(difference(contact, b->center)) - b->radius) <= 1.e-5
        && onArc(*a, contact) && onArc(*b, contact);
}

}  // namespace

SketchSolution solveSketch(const Sketch& sketch, std::span<const SketchProjection> projections)
{
    using enum ConstraintKind;
    validateSketchConstraints(sketch);
    const VariableLayout layout(sketch);
    auto values = layout.pack();
    std::vector<Row> rows;
    const auto add = [&](int index, std::initializer_list<SketchRef> refs, Equation error) {
        Row row{index, {}, std::move(error)};
        for (const auto ref : refs) {
            if (ref.entity < 0) {
                continue;
            }
            const bool spline = std::holds_alternative<Spline>(sketch.entities[ref.entity]);
            const int first = layout.starts[ref.entity] + (spline ? 2 * ref.part : 0);
            const int end = spline ? first + 2 : layout.starts[ref.entity + 1];
            for (int col = first; col < end; ++col) {
                if (std::ranges::find(row.variables, col) == row.variables.end()) {
                    row.variables.push_back(col);
                }
            }
        }
        rows.push_back(std::move(row));
    };
    const SketchRef none{-1, 0};
    const auto join = [&](int index, SketchRef a, SketchRef b, Point2 target = {}) {
        for (bool vertical : {false, true}) {
            add(index, {a, b}, [&layout, a, b, target, vertical](const Values& x) {
                const auto delta = difference(layout.vertex(x, a), b.entity < 0 ? target : layout.vertex(x, b));
                return vertical ? delta.v : delta.u;
            });
        }
    };

    // Endpoints snapped together remain connected when a mixed contour is solved.
    std::map<std::pair<double, double>, SketchRef> ends;
    for (int i = 0; i < static_cast<int>(sketch.entities.size()); ++i) {
        if (isConstruction(sketch, i)) {
            continue;
        }
        for (const int part : sketchEndpoints(sketch.entities[i])) {
            const SketchRef ref{i, part};
            const auto p = layout.vertex(values, ref);
            const auto [at, inserted] = ends.emplace(std::pair{p.u, p.v}, ref);
            if (!inserted && at->second.entity != i) {
                join(-1, at->second, ref);
            }
        }
    }

    SketchSolution result;
    std::vector<std::pair<int, Tangency>> tangencies;
    const auto radius = [&layout](const Values& x, SketchRef ref) {
        const auto round = layout.round(x, ref.entity);
        return round ? round->radius : invalidEquation;
    };
    for (int i = 0; i < static_cast<int>(sketch.constraints.size()); ++i) {
        const auto& item = sketch.constraints[i];
        const auto a = item.first;
        const auto b = item.second;
        const double size = item.value;
        const auto kind = item.kind;
        switch (kind) {
        case Coincident:
            join(i, a, b);
            break;
        case FixPoint:
            join(i, a, none, item.fixed);
            break;
        case OnProjection:
            try {
                join(i, a, none, projectionPoint(item, projections));
            } catch (const std::invalid_argument& error) {
                result.brokenProjections.push_back(i);
                if (result.projectionError.empty()) {
                    result.projectionError = error.what();
                }
            }
            break;
        case Diameter:
            add(i, {a}, [=](const Values& x) { return radius(x, a) * 2 - size; });
            break;
        case Angle:
        case Parallel:
        case Perpendicular: {
            const auto first = layout.vector(values, a);
            const auto second = layout.vector(values, b);
            const auto initial = angle(first, second);
            const auto scale = std::max(1.0, std::min(length(first), length(second)));
            const auto target = kind == Parallel ? (dot(first, second) < 0 ? std::numbers::pi : 0)
                : std::copysign(kind == Angle ? size * std::numbers::pi / 180 : std::numbers::pi / 2, initial);
            add(i, {a, b}, [&, a, b, target, scale](const Values& x) {
                return std::remainder(angle(layout.vector(x, a), layout.vector(x, b)) - target, turn) * scale;
            });
            break;
        }
        case Equal:
            if (roundEntity(sketch.entities[a.entity])) {
                add(i, {a, b}, [=](const Values& x) { return radius(x, a) - radius(x, b); });
            } else {
                add(i, {a, b}, [&, a, b](const Values& x) {
                    return length(layout.vector(x, a)) - length(layout.vector(x, b));
                });
            }
            break;
        case Symmetric: {
            const auto axis = item.axis;
            for (const bool midpoint : {false, true}) {
                add(i, {a, b, axis}, [&, a, b, axis, midpoint](const Values& x) {
                    const auto u = layout.vertex(x, a);
                    const auto v = layout.vertex(x, b);
                    const auto direction = layout.vector(x, axis);
                    const auto norm = length(direction);
                    if (norm <= tolerance) { return invalidEquation; }
                    return midpoint ? cross(direction, difference(scaled(plus(u, v), .5), layout.vertex(x, axis))) / norm
                                    : dot(direction, difference(u, v)) / norm;
                });
            }
            break;
        }
        case Tangent: {
            const auto tangent = describeTangency(layout, values, a, b);
            tangencies.emplace_back(i, tangent);
            add(i, {a, b}, [&layout, tangent](const Values& x) { return tangentError(layout, x, tangent); });
            break;
        }
        case Horizontal:
        case Vertical:
        case Length:
            add(i, {a}, [&, a, kind, size](const Values& x) {
                const auto v = layout.vector(x, a);
                return kind == Horizontal ? v.v : kind == Vertical ? v.u : length(v) - size;
            });
            break;
        }
    }

    solveEquations(rows, values);
    for (const auto& row : rows) {
        if (const double error = row.error(values); !std::isfinite(error) || std::abs(error) > tolerance) {
            result.conflicts.push_back(row.constraint);
        }
    }
    for (const auto& [index, tangent] : tangencies) {
        if (!actualContact(layout, values, tangent)) {
            result.conflicts.push_back(index);
        }
    }
    result.entities.reserve(sketch.entities.size());
    for (int i = 0; i < static_cast<int>(sketch.entities.size()); ++i) {
        result.entities.push_back(layout.shape(values, i));
        if (!validGeometry(result.entities.back())) {
            for (int j = 0; j < static_cast<int>(sketch.constraints.size()); ++j) {
                const auto& item = sketch.constraints[j];
                if (item.first.entity == i || (usesSecondReference(item.kind) && item.second.entity == i)
                    || (item.kind == Symmetric && item.axis.entity == i)) {
                    result.conflicts.push_back(j);
                }
            }
        }
    }
    std::ranges::sort(result.conflicts);
    result.conflicts.erase(std::unique(result.conflicts.begin(), result.conflicts.end()), result.conflicts.end());
    if (result.conflicts.empty() && result.brokenProjections.empty()) {
        degreesOfFreedom(rows, values, layout, result);
    } else {
        result.entities = sketch.entities;
        result.fullyDefined.resize(sketch.entities.size());
    }
    return result;
}

}  // namespace graph3d::geometry
