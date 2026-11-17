#include "drawing/Projection.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <span>
#include <QSet>

namespace graph3d::drawing {
namespace {

enum class Match { Exact, RoundCenter, OuterLine };

QByteArray key(const ProjectedEdge& edge, const View& view, Match match)
{
    constexpr double tolerance = 1.e-5;
    QByteArray result;
    const auto append = [&](double value) {
        result += QByteArray::number(qRound64(value / tolerance)) + ',';
    };
    if (edge.radius > 0) {
        if (match == Match::OuterLine) {
            return {};
        }
        result = "C";
        append(edge.a.x + view.origin.x);
        append(edge.a.y + view.origin.y);
        append(edge.angle);
        append(edge.sweep);
        if (match == Match::Exact) {
            append(edge.radius);
        }
    } else {
        if (match == Match::RoundCenter) {
            return {};
        }
        result = "L";
        if (match == Match::Exact) {
            append(edge.a.x + view.origin.x);
            append(edge.a.y + view.origin.y);
            append(edge.b.x + view.origin.x);
            append(edge.b.y + view.origin.y);
        } else {
            const auto boundary = [](double coordinate, double size) {
                return std::abs(std::abs(coordinate) - size / 2) <= tolerance;
            };
            const bool horizontal = std::abs(edge.a.y - edge.b.y) <= tolerance && boundary(edge.a.y, view.height);
            const bool vertical = std::abs(edge.a.x - edge.b.x) <= tolerance && boundary(edge.a.x, view.width);
            if (!horizontal && !vertical) {
                return {};
            }
            // Only corresponding outer boundaries can move without an exact match.
            append(edge.a.x / view.width);
            append(edge.a.y / view.height);
            append(edge.b.x / view.width);
            append(edge.b.y / view.height);
        }
    }
    return result;
}

void group(QHash<QByteArray, QList<std::size_t>>& groups, const View& view,
           std::span<const char> used, Match match)
{
    for (std::size_t i = 0; i < view.edges.size(); ++i) {
        if (!used[i]) {
            const auto signature = key(view.edges[i], view, match);
            if (!signature.isEmpty()) {
                groups[signature].append(i);
            }
        }
    }
}

}  // namespace

void uniqueProjectionEdges(View& view)
{
    QSet<QByteArray> seen;
    std::erase_if(view.edges, [&](const auto& edge) {
        const auto signature = key(edge, view, Match::Exact);
        if (seen.contains(signature)) {
            return true;
        }
        seen.insert(signature);
        return false;
    });
}

void retainProjectionReferences(View& view, const View& previous)
{
    if (!previous.id.isNull()) {
        view.id = previous.id;
    }
    std::vector<char> claimed(previous.edges.size());
    std::vector<char> assigned(view.edges.size());
    for (const auto match : {Match::Exact, Match::RoundCenter, Match::OuterLine}) {
        QHash<QByteArray, QList<std::size_t>> oldEdges, newEdges;
        group(oldEdges, previous, claimed, match);
        group(newEdges, view, assigned, match);
        for (auto it = newEdges.cbegin(); it != newEdges.cend(); ++it) {
            const auto old = oldEdges.constFind(it.key());
            if (it->size() == 1 && old != oldEdges.cend() && old->size() == 1) {
                view.edges[it->front()].id = previous.edges[old->front()].id;
                claimed[old->front()] = true;
                assigned[it->front()] = true;
            }
        }
    }
}

}  // namespace graph3d::drawing
