#include "drawing/DraftCurves.h"

#include <QLineF>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace graph3d::drawing {
namespace {

constexpr double turn = 2 * std::numbers::pi;
constexpr double degrees = 180 / std::numbers::pi;

double cross(QPointF a, QPointF b)
{
    return a.x() * b.y() - a.y() * b.x();
}

double length(QPointF vector)
{
    return std::hypot(vector.x(), vector.y());
}

double positive(double angle)
{
    return std::fmod(std::fmod(angle, turn) + turn, turn);
}

bool close(QPointF a, QPointF b)
{
    return length(a - b) < draftTolerance;
}

std::optional<QPointF> lineIntersection(const DraftCurve& a, const DraftCurve& b)
{
    QPointF point;
    return QLineF(a.a, a.b).intersects(QLineF(b.a, b.b), &point) == QLineF::NoIntersection
        ? std::nullopt : std::optional(point);
}

void addArc(QPainterPath& path, const DraftCurve& curve)
{
    const QRectF rect(curve.a - QPointF(curve.radius, curve.radius),
                      QSizeF(2 * curve.radius, 2 * curve.radius));
    path.arcTo(rect, -curve.angle * degrees, -curve.sweep * degrees);
}

std::vector<double> intersectionsAlong(const Draft& draft, const DraftItem& item,
                                        const std::vector<DraftCurve>& curves)
{
    std::vector<double> hits;
    for (const auto& other : draft.items) {
        if (!layerVisible(draft, other.layer)) {
            continue;
        }
        if (other.id == item.id) {
            continue;
        }
        for (const auto& edge : draftCurves(other)) {
            for (std::size_t i = 0; i < curves.size(); ++i) {
                for (const auto& hit : draftIntersections(curves[i], edge)) {
                    hits.push_back(i + hit.parameter);
                }
            }
        }
    }
    std::ranges::sort(hits);
    hits.erase(std::unique(hits.begin(), hits.end(),
        [](double a, double b) { return std::abs(a - b) < draftTolerance; }), hits.end());
    return hits;
}

DraftItem arcItem(DraftItem item, const DraftCurve& curve, double first, double last)
{
    item.kind = DraftKind::Arc;
    item.a = curve.at(first);
    item.b = curve.at((first + last) / 2);
    item.label = curve.at(last);
    return item;
}

DraftItem slice(DraftItem item, const std::vector<DraftCurve>& curves, double first, double last)
{
    if (curves.size() == 1) {
        if (curves.front().radius > 0) {
            return arcItem(std::move(item), curves.front(), first, last);
        }
        item.a = curves.front().at(first);
        item.b = curves.front().at(last);
        return item;
    }
    const auto at = [&](double t) {
        if (t >= curves.size() && !close(curves.front().at(0), curves.back().at(1))) {
            return curves.back().at(1);
        }
        const double wrapped = std::fmod(t, curves.size());
        const auto i = static_cast<std::size_t>(wrapped);
        return curves[i].at(wrapped - i);
    };
    item.kind = DraftKind::Polyline;
    item.points = {at(first)};
    for (double i = std::floor(first) + 1; i < last - draftTolerance; ++i) {
        const auto point = at(i);
        if (!close(point, item.points.back())) {
            item.points.push_back(point);
        }
    }
    if (!close(item.points.back(), at(last))) {
        item.points.push_back(at(last));
    }
    return item;
}

std::vector<QPointF> sampled(const std::vector<DraftCurve>& curves)
{
    std::vector<QPointF> points{curves.front().at(0)};
    for (const auto& curve : curves) {
        const int steps = curve.radius > 0
            ? std::clamp(static_cast<int>(std::ceil(std::abs(curve.sweep) * std::sqrt(curve.radius / .02))), 4, 256)
            : 1;
        for (int i = 1; i <= steps; ++i) {
            points.push_back(curve.at(static_cast<double>(i) / steps));
        }
    }
    return points;
}

}  // namespace

QPointF DraftCurve::at(double t) const
{
    if (radius == 0) {
        return a + (b - a) * t;
    }
    const auto angleAt = angle + sweep * t;
    return a + QPointF(std::cos(angleAt), std::sin(angleAt)) * radius;
}

std::optional<double> DraftCurve::parameter(QPointF point) const
{
    if (radius == 0) {
        const auto step = b - a;
        const auto lengthSquared = QPointF::dotProduct(step, step);
        return lengthSquared > 0 ? std::optional(QPointF::dotProduct(point - a, step) / lengthSquared)
                                 : std::nullopt;
    }
    const auto vector = point - a;
    if (graph3d::drawing::length(vector) <= draftTolerance) {
        return {};
    }
    const auto theta = std::atan2(vector.y(), vector.x()) - angle;
    auto relative = positive(sweep > 0 ? theta : -theta);
    if (relative > turn - draftTolerance) {
        relative = 0;
    }
    return relative / std::abs(sweep);
}

double DraftCurve::nearest(QPointF point) const
{
    const double t = parameter(point).value_or(0);
    if (radius > 0 && t > 1) {
        return graph3d::drawing::length(point - at(0)) <= graph3d::drawing::length(point - at(1)) ? 0 : 1;
    }
    return std::clamp(t, 0.0, 1.0);
}

double DraftCurve::length() const
{
    return radius > 0 ? radius * std::abs(sweep) : graph3d::drawing::length(b - a);
}

DraftCurve draftArc(QPointF start, QPointF through, QPointF end)
{
    const auto u = through - start;
    const auto v = end - start;
    const double determinant = 2 * cross(u, v);
    if (std::abs(determinant) <= draftTolerance * std::max(1.0, length(u) * length(v))) {
        return {};
    }
    const double uu = QPointF::dotProduct(u, u);
    const double vv = QPointF::dotProduct(v, v);
    const auto relative = QPointF(uu * v.y() - vv * u.y(), u.x() * vv - v.x() * uu) / determinant;
    const auto center = start + relative;
    const auto angleAt = [center](QPointF point) {
        const auto direction = point - center;
        return std::atan2(direction.y(), direction.x());
    };
    const double angle = angleAt(start);
    const double endAngle = positive(angleAt(end) - angle);
    const double sweep = positive(angleAt(through) - angle) <= endAngle ? endAngle : endAngle - turn;
    return {center, {}, length(relative), angle, sweep};
}

std::vector<DraftCurve> draftCurves(const DraftItem& item)
{
    switch (item.kind) {
    case DraftKind::Line: return {{item.a, item.b}};
    case DraftKind::Circle: return {{item.a, {}, QLineF(item.a, item.b).length(), 0, turn}};
    case DraftKind::Arc: {
        const auto arc = draftArc(item.a, item.b, item.label);
        return arc.radius > 0 ? std::vector{arc} : std::vector<DraftCurve>{};
    }
    case DraftKind::Rectangle: {
        const QPointF other1(item.b.x(), item.a.y()), other2(item.a.x(), item.b.y());
        return {{item.a, other1}, {other1, item.b}, {item.b, other2}, {other2, item.a}};
    }
    case DraftKind::Polyline: {
        std::vector<DraftCurve> segments;
        for (std::size_t i = 1; i < item.points.size(); ++i) {
            segments.push_back({item.points[i - 1], item.points[i]});
        }
        return segments;
    }
    default: return {};
    }
}

QPainterPath draftCurvePath(const DraftItem& item)
{
    const auto curves = draftCurves(item);
    QPainterPath path;
    if (!curves.empty()) {
        path.moveTo(curves.front().at(0));
        for (const auto& edge : curves) {
            if (edge.radius > 0) {
                addArc(path, edge);
            } else {
                path.lineTo(edge.b);
            }
        }
    }
    return path;
}

std::vector<DraftCrossing> draftIntersections(const DraftCurve& a, const DraftCurve& b, bool extendFirst)
{
    std::vector<QPointF> candidates;
    if (a.radius == 0 && b.radius == 0) {
        if (const auto point = lineIntersection(a, b)) {
            candidates.push_back(*point);
        }
    } else if (a.radius > 0 && b.radius > 0) {
        const auto delta = b.a - a.a;
        const auto d = length(delta);
        if (d < draftTolerance || d > a.radius + b.radius + draftTolerance
            || d < std::abs(a.radius - b.radius) - draftTolerance) {
            return {};
        }
        const auto unit = delta / d;
        const auto along = (a.radius * a.radius - b.radius * b.radius + d * d) / (2 * d);
        const auto height = std::sqrt(std::max(0.0, a.radius * a.radius - along * along));
        const auto foot = a.a + unit * along;
        const QPointF perpendicular(-unit.y(), unit.x());
        candidates = {foot + perpendicular * height, foot - perpendicular * height};
    } else {
        const auto& line = a.radius == 0 ? a : b;
        const auto& circle = a.radius > 0 ? a : b;
        const auto step = line.b - line.a;
        const auto d = length(step);
        if (d < draftTolerance) {
            return {};
        }
        const auto unit = step / d;
        const auto foot = line.a + unit * QPointF::dotProduct(circle.a - line.a, unit);
        const auto distance = length(foot - circle.a);
        if (distance > circle.radius + draftTolerance) {
            return {};
        }
        const auto along = std::sqrt(std::max(0.0, circle.radius * circle.radius - distance * distance));
        candidates = {foot - unit * along, foot + unit * along};
    }

    std::vector<DraftCrossing> result;
    for (const auto point : candidates) {
        const auto u = a.parameter(point);
        const auto v = b.parameter(point);
        if (u && v && *v >= -draftTolerance && *v <= 1 + draftTolerance
            && (extendFirst || (*u >= -draftTolerance && *u <= 1 + draftTolerance))
            && (result.empty() || !close(result.front().point, point))) {
            result.push_back({extendFirst ? *u : std::clamp(*u, 0.0, 1.0), point});
        }
    }
    return result;
}

std::vector<DraftItem> trimDraft(const DraftItem& item, const Draft& draft, QPointF click)
{
    const auto curves = draftCurves(item);
    if (curves.empty()) {
        return {};
    }
    const double count = static_cast<double>(curves.size());
    const bool closed = close(curves.front().at(0), curves.back().at(1));
    auto hits = intersectionsAlong(draft, item, curves);
    const bool atJoin = !hits.empty() && (hits.front() < draftTolerance || hits.back() > count - draftTolerance);
    std::erase_if(hits, [=](double t) { return t < draftTolerance || t > count - draftTolerance; });
    if (closed && atJoin) {
        hits.insert(hits.begin(), 0);
    }
    if (hits.size() < (closed ? 2 : 1)) {
        return {};
    }
    double parameter = 0;
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const auto t = curves[i].nearest(click);
        const auto distance = length(curves[i].at(t) - click);
        if (distance < best) {
            best = distance;
            parameter = i + t;
        }
    }
    const auto next = std::upper_bound(hits.begin(), hits.end(), parameter);
    std::vector<DraftItem> pieces;
    if (closed) {
        const double after = next == hits.end() ? hits.front() : *next;
        const double before = next == hits.begin() ? hits.back() : *std::prev(next);
        pieces.push_back(slice(item, curves, after, before <= after ? before + count : before));
    } else {
        if (next != hits.begin()) {
            pieces.push_back(slice(item, curves, 0, *std::prev(next)));
        }
        if (next != hits.end()) {
            pieces.push_back(slice(item, curves, *next, count));
        }
    }
    if (pieces.size() == 2) {
        pieces[1].id = QUuid::createUuid();
    }
    return std::ranges::all_of(pieces, validItem) ? pieces : std::vector<DraftItem>{};
}

std::optional<DraftItem> extendDraft(const DraftItem& item, const Draft& draft, QPointF click)
{
    const auto curves = draftCurves(item);
    if (curves.empty() || close(curves.front().at(0), curves.back().at(1))) {
        return {};
    }
    const bool start = length(click - curves.front().at(0)) < length(click - curves.back().at(1));
    const auto& edge = start ? curves.front() : curves.back();
    double limit = std::numeric_limits<double>::infinity();
    std::optional<double> choice;
    for (const auto& other : draft.items) {
        if (!layerVisible(draft, other.layer)) {
            continue;
        }
        if (item.id == other.id) {
            continue;
        }
        for (const auto& target : draftCurves(other)) {
            for (const auto& hit : draftIntersections(edge, target, true)) {
                double t = hit.parameter;
                if (edge.radius > 0 && start && t > 1 + draftTolerance) {
                    t -= turn / std::abs(edge.sweep);
                }
                const double distance = start ? -t : t - 1;
                if (distance > draftTolerance && distance < limit) {
                    choice = t;
                    limit = distance;
                }
            }
        }
    }
    if (!choice) {
        return {};
    }
    auto after = item;
    const auto point = edge.at(*choice);
    if (item.kind == DraftKind::Arc) {
        after = arcItem(item, edge, start ? *choice : 0, start ? 1 : *choice);
    } else if (item.kind == DraftKind::Polyline) {
        (start ? after.points.front() : after.points.back()) = point;
    } else {
        (start ? after.a : after.b) = point;
    }
    return validItem(after) ? std::optional(std::move(after)) : std::nullopt;
}

std::optional<DraftItem> offsetDraft(const DraftItem& item, double distance)
{
    auto curves = draftCurves(item);
    if (curves.empty() || std::abs(distance) < draftTolerance || !std::isfinite(distance)) {
        return {};
    }
    auto after = item;
    if (curves.front().radius > 0) {
        auto curve = curves.front();
        curve.radius += distance;
        if (curve.radius < draftTolerance) {
            return {};
        }
        if (item.kind == DraftKind::Circle) {
            after.b = curve.at(0);
        } else {
            after = arcItem(item, curve, 0, 1);
        }
    } else {
        const bool closed = close(curves.front().at(0), curves.back().at(1));
        for (auto& line : curves) {
            const auto vector = line.b - line.a;
            const auto shift = QPointF(vector.y(), -vector.x()) * (distance / length(vector));
            line.a += shift;
            line.b += shift;
        }
        if (item.kind == DraftKind::Line) {
            after.a = curves.front().a;
            after.b = curves.front().b;
        } else {
            after.kind = DraftKind::Polyline;
            after.points.clear();
            if (!closed) {
                after.points.push_back(curves.front().a);
            }
            const auto joint = [&](const DraftCurve& first, const DraftCurve& second) {
                auto point = lineIntersection(first, second);
                if (!point && close(first.b, second.a)) {
                    point = first.b;
                }
                return point;
            };
            for (std::size_t i = closed ? 0 : 1; i < curves.size(); ++i) {
                const auto previous = (i + curves.size() - 1) % curves.size();
                const auto point = joint(curves[previous], curves[i]);
                if (!point) {
                    return {};
                }
                after.points.push_back(*point);
            }
            after.points.push_back(closed ? after.points.front() : curves.back().b);
        }
    }
    after.id = QUuid{};
    return validItem(after) ? std::optional(std::move(after)) : std::nullopt;
}

std::optional<DraftItem> hatchDraft(const Draft& draft, QPointF point)
{
    std::vector<std::vector<QPointF>> closed, open;
    for (const auto& item : draft.items) {
        if (!layerVisible(draft, item.layer)) {
            continue;
        }
        const auto curves = draftCurves(item);
        if (!curves.empty()) {
            auto points = sampled(curves);
            auto& pool = close(points.front(), points.back()) ? closed : open;
            pool.push_back(std::move(points));
        }
    }
    for (std::size_t i = 0; i < open.size(); ++i) {
        auto loop = std::move(open[i]);
        if (loop.empty()) {
            continue;
        }
        while (!close(loop.front(), loop.back())) {
            std::optional<std::size_t> next;
            bool reverse = false;
            bool ambiguous = false;
            for (std::size_t j = i + 1; j < open.size(); ++j) {
                if (open[j].empty()) {
                    continue;
                }
                const bool atStart = close(loop.back(), open[j].front());
                const bool atEnd = close(loop.back(), open[j].back());
                if (atStart || atEnd) {
                    if (next) {
                        ambiguous = true;
                        break;
                    }
                    next = j;
                    reverse = atEnd;
                }
            }
            if (ambiguous || !next) {
                break;
            }
            auto& edge = open[*next];
            if (reverse) {
                std::ranges::reverse(edge);
            }
            loop.insert(loop.end(), std::next(edge.begin()), edge.end());
            edge.clear();
        }
        if (loop.size() >= 4 && close(loop.front(), loop.back())) {
            closed.push_back(std::move(loop));
        }
    }

    const auto path = [](const auto& loop) {
        QPainterPath result;
        result.addPolygon(QPolygonF(loop.begin(), loop.end()));
        result.closeSubpath();
        return result;
    };
    std::optional<std::size_t> region;
    QPainterPath outer;
    for (std::size_t i = 0; i < closed.size(); ++i) {
        auto candidate = path(closed[i]);
        if (candidate.contains(point) && (!region || outer.contains(candidate))) {
            region = i;
            outer = std::move(candidate);
        }
    }
    if (!region) {
        return {};
    }
    DraftItem result;
    result.kind = DraftKind::Hatch;
    for (std::size_t i = 0; i < closed.size(); ++i) {
        if (i == *region || outer.contains(path(closed[i]))) {
            result.loops.push_back(std::move(closed[i]));
        }
    }
    return validItem(result) ? std::optional(std::move(result)) : std::nullopt;
}

}  // namespace graph3d::drawing
