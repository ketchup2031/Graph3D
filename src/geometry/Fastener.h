#pragma once

#include <span>
#include <string>
#include <vector>
#include <TopoDS_Shape.hxx>

namespace graph3d::geometry {

enum class FastenerKind { Bolt, Nut, Washer };
struct Fastener {
    FastenerKind kind = FastenerKind::Bolt;
    int diameter = 8;
    int length = 25;
    bool operator==(const Fastener&) const = default;
};
struct FastenerSize {
    int diameter;
    double pitch, wrench, head, nut, washerBore, washerOuter, washerThickness;
    std::span<const int> lengths;
};
std::span<const FastenerSize> fastenerCatalog();
const FastenerSize& fastenerSize(Fastener item);
std::string fastenerDesignation(Fastener item);
TopoDS_Shape makeFastener(Fastener item);

}  // namespace graph3d::geometry
