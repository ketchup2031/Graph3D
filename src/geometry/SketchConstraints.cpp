#include "geometry/SketchConstraints.h"

#include <GeomAbs_CurveType.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
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
    std::array<int, 8> variables{};
    int count = 0;
    Equation error;
};

double dot(const Values& a, const Values& b)
{
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0);
}

void solveEquations(const std::vector<Row>& rows, Values& values)
{
    // Solve the linearized constraints together. The damping chooses a small
    // change when a sketch has free coordinates and regularizes redundant rows.
    constexpr double damping = 1.e-8;
    std::vector<std::array<double, 8>> derivatives(rows.size());
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
            for (int k = 0; k < row.count; ++k) {
                const int index = row.variables[k];
                auto& x = values[index];
                const double saved = x;
                const double difference = std::max(1.e-5, 1.e-8 * std::abs(saved));
                x = saved + difference;
                const double high = row.error(values);
                x = saved - difference;
                const double low = row.error(values);
                x = saved;
                const double partial = (high - low) / (2 * difference);
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
                for (int k = 0; k < row.count; ++k) {
                    component += derivatives[i][k] * direction[row.variables[k]];
                }
                for (int k = 0; k < row.count; ++k) {
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

Point2 vertex(const Values& values, SketchRef ref, bool rectangle)
{
    const auto base = ref.entity * 4;
    if (rectangle) {
        return {values[base + (ref.part == 1 || ref.part == 2 ? 2 : 0)],
                values[base + (ref.part == 2 || ref.part == 3 ? 3 : 1)]};
    }
    const auto slot = base + 2 * ref.part;
    return {values[slot], values[slot + 1]};
}

Point2 segment(const Values& values, SketchRef ref, bool rectangle)
{
    const auto first = vertex(values, ref, rectangle);
    ref.part = rectangle ? (ref.part + 1) % 4 : 1;
    return difference(vertex(values, ref, rectangle), first);
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
    return std::holds_alternative<Rectangle>(entity) ? 4 : std::holds_alternative<Line>(entity) ? 2 : 1;
}

int sketchSegmentCount(const SketchEntity& entity)
{
    return std::holds_alternative<Rectangle>(entity) ? 4 : std::holds_alternative<Line>(entity) ? 1 : 0;
}

Point2 sketchVertex(const SketchEntity& entity, int part)
{
    if (part < 0 || part >= sketchVertexCount(entity)) {
        throw std::invalid_argument("Invalid sketch point reference.");
    }
    if (const auto* line = std::get_if<Line>(&entity)) {
        return part == 0 ? line->start : line->end;
    }
    if (const auto* rectangle = std::get_if<Rectangle>(&entity)) {
        return {part == 1 || part == 2 ? rectangle->opposite.u : rectangle->first.u,
                part == 2 || part == 3 ? rectangle->opposite.v : rectangle->first.v};
    }
    return std::get<Circle>(entity).center;
}

Line sketchSegment(const SketchEntity& entity, int part)
{
    if (part < 0 || part >= sketchSegmentCount(entity)) {
        throw std::invalid_argument("Invalid sketch segment reference.");
    }
    return {sketchVertex(entity, part), sketchVertex(entity, (part + 1) % sketchVertexCount(entity))};
}

void validateSketchConstraints(const Sketch& sketch)
{
    if (sketch.constraints.size() > maxConstraints) {
        throw std::invalid_argument("Too many sketch constraints.");
    }
    const auto entity = [&](SketchRef ref) -> const SketchEntity& {
        if (ref.entity < 0 || static_cast<std::size_t>(ref.entity) >= sketch.entities.size()) {
            throw std::invalid_argument("Invalid sketch entity reference.");
        }
        return sketch.entities[ref.entity];
    };
    for (const auto& relation : sketch.constraints) {
        const auto& first = entity(relation.first);
        const auto requireValue = [&] {
            if (!std::isfinite(relation.value) || relation.value <= tolerance || relation.value > coordinateLimit) {
                throw std::invalid_argument("Enter a positive sketch dimension.");
            }
        };
        switch (relation.kind) {
        case ConstraintKind::Coincident:
            sketchVertex(first, relation.first.part);
            sketchVertex(entity(relation.second), relation.second.part);
            break;
        case ConstraintKind::Perpendicular:
            sketchSegment(entity(relation.second), relation.second.part);
            [[fallthrough]];
        case ConstraintKind::Horizontal:
        case ConstraintKind::Vertical:
        case ConstraintKind::Length:
            sketchSegment(first, relation.first.part);
            if (relation.kind == ConstraintKind::Length) {
                requireValue();
            }
            break;
        case ConstraintKind::Diameter:
            if (!std::holds_alternative<Circle>(first) || relation.first.part != 0) {
                throw std::invalid_argument("Select a circle for a diameter constraint.");
            }
            requireValue();
            break;
        case ConstraintKind::OnProjection:
            sketchVertex(first, relation.first.part);
            if (!sketch.support || !relation.edge || !std::isfinite(relation.value)
                || relation.value < 0 || relation.value > 1) {
                throw std::invalid_argument("Invalid projected sketch edge reference.");
            }
            break;
        }
    }
}

SketchSolution solveSketch(const Sketch& sketch, std::span<const SketchProjection> projections)
{
    validateSketchConstraints(sketch);
    if (sketch.constraints.empty()) {
        return {sketch.entities, {}};
    }
    Values values;
    values.reserve(sketch.entities.size() * 4);
    std::vector<bool> rectangle;
    for (const auto& entity : sketch.entities) {
        rectangle.push_back(std::holds_alternative<Rectangle>(entity));
        const auto p = sketchVertex(entity, 0);
        if (const auto* circle = std::get_if<Circle>(&entity)) {
            values.insert(values.end(), {p.u, p.v, circle->radius, 0});
        } else {
            const auto other = sketchVertex(entity, rectangle.back() ? 2 : 1);
            values.insert(values.end(), {p.u, p.v, other.u, other.v});
        }
    }
    if (!std::ranges::all_of(values, [](double value) {
        return std::isfinite(value) && std::abs(value) <= coordinateLimit;
    })) {
        throw std::invalid_argument("The profile has invalid coordinates.");
    }
    std::vector<Row> rows;
    const auto add = [&](int index, SketchRef a, SketchRef b, Equation error) {
        Row row{index, {}, 0, std::move(error)};
        for (int entity : {a.entity, b.entity}) {
            if (entity < 0 || (row.count > 0 && entity == a.entity)) {
                continue;
            }
            for (int i = 0; i < 4; ++i) {
                row.variables[row.count++] = entity * 4 + i;
            }
        }
        rows.push_back(std::move(row));
    };
    const auto join = [&](int index, SketchRef a, SketchRef b, Point2 target = {}) {
        const bool aRect = rectangle[a.entity];
        const bool bRect = b.entity >= 0 && rectangle[b.entity];
        for (bool vertical : {false, true}) {
            add(index, a, b, [=](const Values& x) {
                const auto delta = difference(vertex(x, a, aRect), b.entity < 0 ? target : vertex(x, b, bRect));
                return vertical ? delta.v : delta.u;
            });
        }
    };
    // Legacy polylines use individually stored lines. Preserve touching ends
    // during updates, including when a length is applied to just one segment.
    std::map<std::pair<double, double>, SketchRef> ends;
    for (int i = 0; i < static_cast<int>(sketch.entities.size()); ++i) {
        if (!std::holds_alternative<Line>(sketch.entities[i])) {
            continue;
        }
        for (int part : {0, 1}) {
            const SketchRef ref{i, part};
            const auto p = vertex(values, ref, false);
            const auto [at, inserted] = ends.emplace(std::pair{p.u, p.v}, ref);
            if (!inserted && at->second.entity != i) {
                join(-1, at->second, ref);
            }
        }
    }

    SketchSolution result;
    for (int i = 0; i < static_cast<int>(sketch.constraints.size()); ++i) {
        const auto& relation = sketch.constraints[i];
        const auto a = relation.first;
        const auto b = relation.second;
        const SketchRef none{-1, 0};
        const bool aRect = rectangle[a.entity];
        const double size = relation.value;
        switch (relation.kind) {
        case ConstraintKind::Coincident:
            join(i, a, b);
            break;
        case ConstraintKind::OnProjection:
            try {
                join(i, a, none, projectionPoint(relation, projections));
            } catch (const std::invalid_argument& error) {
                result.brokenProjections.push_back(i);
                if (result.projectionError.empty()) {
                    result.projectionError = error.what();
                }
            }
            break;
        case ConstraintKind::Diameter:
            add(i, a, none, [=](const Values& x) { return x[a.entity * 4 + 2] * 2 - size; });
            break;
        case ConstraintKind::Perpendicular: {
            const bool bRect = rectangle[b.entity];
            const auto first = segment(values, a, aRect);
            const auto second = segment(values, b, bRect);
            const double target = std::copysign(std::numbers::pi / 2, angle(first, second));
            const double scale = std::max(1.0, std::min(length(first), length(second)));
            add(i, a, b, [=](const Values& x) {
                return std::remainder(angle(segment(x, a, aRect), segment(x, b, bRect)) - target,
                                      2 * std::numbers::pi) * scale;
            });
            break;
        }
        default:
            add(i, a, none, [=](const Values& x) {
                const auto vector = segment(x, a, aRect);
                if (relation.kind == ConstraintKind::Horizontal) {
                    return vector.v;
                }
                if (relation.kind == ConstraintKind::Vertical) {
                    return vector.u;
                }
                return length(vector) - size;
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
    for (int i = 0; i < static_cast<int>(sketch.constraints.size()); ++i) {
        const auto& item = sketch.constraints[i];
        const auto a = item.first;
        const auto b = item.second;
        const bool collapsed = (item.kind == ConstraintKind::Horizontal || item.kind == ConstraintKind::Vertical
            || item.kind == ConstraintKind::Length || item.kind == ConstraintKind::Perpendicular)
            && length(segment(values, a, rectangle[a.entity])) <= tolerance;
        if (collapsed || (item.kind == ConstraintKind::Perpendicular
            && length(segment(values, b, rectangle[b.entity])) <= tolerance)) {
            result.conflicts.push_back(i);
        }
    }
    std::ranges::sort(result.conflicts);
    result.conflicts.erase(std::unique(result.conflicts.begin(), result.conflicts.end()), result.conflicts.end());
    result.entities.reserve(sketch.entities.size());
    for (int i = 0; i < static_cast<int>(sketch.entities.size()); ++i) {
        const auto p = vertex(values, {i, 0}, rectangle[i]);
        if (rectangle[i]) {
            result.entities.emplace_back(Rectangle{p, vertex(values, {i, 2}, true)});
        } else if (std::holds_alternative<Line>(sketch.entities[i])) {
            result.entities.emplace_back(Line{p, vertex(values, {i, 1}, false)});
        } else {
            result.entities.emplace_back(Circle{p, values[i * 4 + 2]});
        }
    }
    return result;
}

}  // namespace graph3d::geometry
