#include "geometry/PreviewBox.h"

#include <array>
#include <cmath>
#include <iostream>
#include <set>
#include <utility>

int main()
{
    const auto edges = graph3d::geometry::makePreviewBox();
    std::set<std::pair<int, int>> uniqueEdges;
    auto cornerIndex = [](const graph3d::geometry::Point& p) {
        const std::array coordinates{p.x, p.y, p.z};
        int index = 0;
        for (double value : coordinates) {
            index *= 2;
            if (std::abs(value - 100.0) < 1e-7) {
                ++index;
            } else if (!(std::abs(value) < 1e-7)) {
                return -1;
            }
        }
        return index;
    };

    for (const auto& edge : edges) {
        int a = cornerIndex(edge.start);
        int b = cornerIndex(edge.end);
        if (a > b) {
            std::swap(a, b);
        }
        const int changedAxes = a ^ b;
        if (a < 0 || (changedAxes != 1 && changedAxes != 2 && changedAxes != 4)
            || !uniqueEdges.emplace(a, b).second) {
            std::cerr << "Expected unique axis-aligned edges between cube corners.\n";
            return 1;
        }
    }

    if (uniqueEdges.size() != 12) {
        std::cerr << "Expected all 12 cube edges.\n";
        return 1;
    }
}
