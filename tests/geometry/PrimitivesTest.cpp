#include "geometry/Primitives.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <utility>

int main()
{
    using graph3d::geometry::Primitive;
    constexpr double pi = std::numbers::pi;
    const std::array cases{
        std::pair{Primitive::Cube, 1'000'000.0},
        std::pair{Primitive::Cylinder, 250'000.0 * pi},
        std::pair{Primitive::Sphere, 4.0 / 3.0 * 125'000.0 * pi},
    };

    for (const auto& [primitive, expectedVolume] : cases) {
        const auto shape = graph3d::geometry::makePrimitive(primitive, 140.0, -280.0);
        GProp_GProps properties;
        BRepGProp::VolumeProperties(shape, properties);
        const auto center = properties.CentreOfMass();
        if (!(std::abs(properties.Mass() - expectedVolume) < expectedVolume * 1e-7)
            || !center.IsEqual(gp_Pnt(190.0, -230.0, 50.0), 1e-7)) {
            std::cerr << "Unexpected volume or position for primitive "
                      << static_cast<int>(primitive) << '\n';
            return 1;
        }
    }
}
