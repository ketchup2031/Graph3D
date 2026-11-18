#include "geometry/SketchCurves.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace graph3d::geometry {

std::optional<ArcGeometry> sketchArc(const Arc& arc)
{
    const auto& [a, b, c] = arc;
    const Point2 u{b.u - a.u, b.v - a.v};
    const Point2 v{c.u - a.u, c.v - a.v};
    const double cross = u.u * v.v - u.v * v.u;
    const double u2 = u.u * u.u + u.v * u.v;
    const double v2 = v.u * v.u + v.v * v.v;
    if (!std::isfinite(cross) || std::abs(cross) <= 1.e-12 * std::max(u2, v2)) {
        return {};
    }
    const Point2 offset{(u2 * v.v - v2 * u.v) / (2 * cross), (u.u * v2 - v.u * u2) / (2 * cross)};
    const Point2 center{a.u + offset.u, a.v + offset.v};
    const double radius = std::hypot(offset.u, offset.v);
    const double start = std::atan2(-offset.v, -offset.u);
    constexpr double turn = 2 * std::numbers::pi;
    const auto angle = [&](Point2 point) {
        double delta = std::atan2(point.v - center.v, point.u - center.u) - start;
        return delta < 0 ? delta + turn : delta;
    };
    const auto finish = angle(c);
    const auto sweep = angle(b) <= finish ? finish : finish - turn;
    if (!std::isfinite(radius) || radius <= 1.e-7 || std::abs(sweep) <= 1.e-10) {
        return {};
    }
    return ArcGeometry{center, radius, start, sweep};
}

std::vector<std::array<Point2, 4>> sketchSplineSpans(const Spline& spline)
{
    const auto& points = spline.points;
    if (points.size() < 2 || (spline.closed && points.size() < 3)) {
        return {};
    }
    // Uniform interpolating cubic, with extrapolated tangents at the two open ends.
    std::vector<std::array<Point2, 4>> spans;
    const auto n = points.size();
    const auto count = spline.closed ? n : n - 1;
    for (std::size_t i = 0; i < count; ++i) {
        const auto a = points[i];
        const auto b = points[(i + 1) % n];
        const auto prev = i > 0 ? points[i - 1] : spline.closed ? points.back()
            : Point2{2 * a.u - b.u, 2 * a.v - b.v};
        const auto next = i + 2 < n ? points[i + 2] : spline.closed ? points[(i + 2) % n]
            : Point2{2 * b.u - a.u, 2 * b.v - a.v};
        spans.push_back({a, {a.u + (b.u - prev.u) / 6, a.v + (b.v - prev.v) / 6},
                            {b.u - (next.u - a.u) / 6, b.v - (next.v - a.v) / 6}, b});
    }
    return spans;
}

}  // namespace graph3d::geometry
