#include "geometry/Primitives.h"
#include "geometry/SketchConstraints.h"
#include "geometry/SketchCurves.h"
#include "geometry/SketchModeling.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>

#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>

using namespace graph3d::geometry;

namespace {

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void volumeIs(const TopoDS_Shape& shape, double volume, std::string_view message)
{
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    expect(std::abs(properties.Mass() - volume) < volume * 1e-6, message);
}

template<class Callable>
void rejected(Callable operation, std::string_view message)
{
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

}  // namespace

int main()
{
    try {
        for (const auto plane : {Plane::XY, Plane::XZ, Plane::YZ}) {
            const Sketch sketch{plane, {Rectangle{{0, 0}, {20, 10}}}};
            const auto face = makeProfile(sketch);
            const auto solid = extrude(face, plane, -15.0);
            volumeIs(solid, 3000.0, "Rectangle extrusion has the wrong volume");
            GProp_GProps center;
            BRepGProp::VolumeProperties(solid, center);
            const auto p = center.CentreOfMass();
            const auto expected = plane == Plane::XY ? gp_Pnt(10, 5, -7.5)
                                : plane == Plane::XZ ? gp_Pnt(10, 7.5, 5)
                                                     : gp_Pnt(-7.5, 10, 5);
            expect(p.IsEqual(expected, 1e-7), "Extrusion does not follow the plane normal");
        }
        const Sketch circle{Plane::XY, {Circle{{0, 0}, 10}}};
        volumeIs(extrude(makeProfile(circle), circle.plane, 20), 2000 * std::numbers::pi,
                 "Circle does not extrude to the expected cylinder");

        const Sketch loop{Plane::XY, {Line{{0, 0}, {20, 0}}, Line{{20, 0}, {0, 10}}, Line{{0, 10}, {0, 0}}}};
        volumeIs(extrude(makeProfile(loop), loop.plane, 10), 1000.0,
                 "Three connected lines did not produce a triangular prism");
        const Sketch open{Plane::XY, {Line{{0, 0}, {20, 0}}, Line{{20, 0}, {0, 10}}}};
        rejected([&] { makeProfile(open); }, "An open profile was accepted");
        const Sketch crossing{Plane::XY, {Line{{0, 0}, {10, 10}}, Line{{10, 10}, {0, 10}},
                                         Line{{0, 10}, {10, 0}}, Line{{10, 0}, {0, 0}}}};
        rejected([&] { makeProfile(crossing); }, "A crossing profile was accepted");
        const Sketch folded{Plane::XY, {Line{{0, 0}, {10, 0}}, Line{{10, 0}, {0, 0}}}};
        rejected([&] { makeProfile(folded); }, "A repeated zero-area boundary was accepted");
        const Sketch mixed{Plane::XY, {Rectangle{{0, 0}, {20, 20}}, Circle{{10, 10}, 5}}};
        volumeIs(extrude(makeProfile(mixed), mixed.plane, 10), (400 - 25 * std::numbers::pi) * 10,
                 "Inner circular contour did not cut the extrusion");
        auto disjoint = mixed;
        disjoint.entities.push_back(Rectangle{{30, 30}, {40, 40}});
        rejected([&] { makeProfile(disjoint); }, "A second outer contour was accepted");
        auto touching = mixed;
        touching.entities[1] = Circle{{5, 10}, 5};
        rejected([&] { makeProfile(touching); }, "An inner circle touching the outer boundary was accepted");
        auto overlapping = mixed;
        overlapping.entities.push_back(Circle{{12, 12}, 5});
        rejected([&] { makeProfile(overlapping); }, "Intersecting inner contours were accepted");

        auto parametric = mixed;
        parametric.constraints = {{ConstraintKind::Length, {0, 0}, {}, 30},
                                  {ConstraintKind::Diameter, {1, 0}, {}, 12}};
        volumeIs(extrude(makeProfile(parametric), parametric.plane, 10), (600 - 36 * std::numbers::pi) * 10,
                 "Driving dimensions did not change both the profile width and the hole");
        parametric.constraints.push_back({ConstraintKind::Length, {0, 0}, {}, 45});
        expect(!solveSketch(parametric).conflicts.empty(), "Conflicting driving dimensions were not reported");
        rejected([&] { makeProfile(parametric); }, "Conflicting sketch was accepted");

        const Sketch constrainedLines{Plane::XY,
            {Line{{0, 0}, {12, 2}}, Line{{13, 2}, {15, 15}}, Line{{40, 12}, {43, 21}}},
            {{ConstraintKind::Coincident, {0, 1}, {1, 0}}, {ConstraintKind::Horizontal, {0, 0}},
             {ConstraintKind::Perpendicular, {0, 0}, {1, 0}}, {ConstraintKind::Vertical, {2, 0}}}};
        const auto solution = solveSketch(constrainedLines);
        expect(solution.conflicts.empty(), "A consistent set of geometric constraints was rejected");
        const auto firstEdge = std::get<Line>(solution.entities[0]);
        const auto secondEdge = std::get<Line>(solution.entities[1]);
        const auto thirdEdge = std::get<Line>(solution.entities[2]);
        expect(std::abs(firstEdge.start.v - firstEdge.end.v) < 1.e-7
            && std::hypot(firstEdge.end.u - secondEdge.start.u, firstEdge.end.v - secondEdge.start.v) < 1.e-7
            && std::abs(secondEdge.start.u - secondEdge.end.u) < 1.e-7
            && std::abs(thirdEdge.start.u - thirdEdge.end.u) < 1.e-7, "A geometric relation was not satisfied");

        Sketch freedom{Plane::XY, {Line{{0, 0}, {20, 0}}, Circle{{25, 8}, 4}}, {}};
        expect(solveSketch(freedom).degreesOfFreedom == 7, "Free line and circle have seven degrees of freedom");
        freedom.constraints = {{ConstraintKind::Horizontal, {0, 0}}, {ConstraintKind::Length, {0, 0}, {}, 20},
                               {ConstraintKind::FixPoint, {0, 0}}};
        const auto fixed = solveSketch(freedom);
        expect(fixed.conflicts.empty() && fixed.degreesOfFreedom == 3 && fixed.fullyDefined[0]
            && !fixed.fullyDefined[1], "The fully constrained line and the free circle were not distinguished");

        Sketch relations{Plane::XY, {Line{{0, 0}, {8, 1}}, Line{{1, 10}, {11, 12}},
                                     Line{{0, -20}, {0, 20}}, Circle{{-10, 8}, 3}, Circle{{12, 9}, 5}},
            {{ConstraintKind::Parallel, {0, 0}, {1, 0}}, {ConstraintKind::Equal, {0, 0}, {1, 0}},
             {ConstraintKind::Equal, {3, 0}, {4, 0}}, {ConstraintKind::Symmetric, {3, 0}, {4, 0}, 0, {}, {2, 0}},
             {ConstraintKind::FixPoint, {2, 0}, {}, 0, {}, {}, {0, -20}},
             {ConstraintKind::FixPoint, {2, 1}, {}, 0, {}, {}, {0, 20}}}, {}, 0, {2}};
        const auto relation = solveSketch(relations);
        expect(relation.conflicts.empty() && relation.degreesOfFreedom == 9 && relation.fullyDefined[2],
            "Parallelism, equality and symmetry did not resolve");
        const auto left = std::get<Circle>(relation.entities[3]);
        const auto right = std::get<Circle>(relation.entities[4]);
        expect(std::abs(left.center.u + right.center.u) < 1.e-6 && std::abs(left.center.v - right.center.v) < 1.e-6
            && std::abs(left.radius - right.radius) < 1.e-6, "Symmetric circles did not retain equal radii");

        const Sketch curved{Plane::XY, {Spline{{{0, 0}, {5, 12}, {15, 12}, {20, 0}}},
                                      Line{{20, 0}, {0, 0}}, Line{{-45, -7}, {45, -7}}}, {}, {}, 0, {2}};
        const auto splineBody = extrude(makeProfile(curved), curved.plane, 10);
        GProp_GProps splineProperties;
        BRepGProp::VolumeProperties(splineBody, splineProperties);
        expect(splineProperties.Mass() > 1800, "A spline and a line did not form a profile");
        const Sketch closedSpline{Plane::XY, {Spline{{{0, 0}, {20, 0}, {20, 20}, {0, 20}}, true}}};
        BRepGProp::VolumeProperties(extrude(makeProfile(closedSpline), closedSpline.plane, 10), splineProperties);
        expect(splineProperties.Mass() > 4000, "A closed spline did not form a profile");

        const Sketch radial{Plane::XZ, {Rectangle{{10, 0}, {20, 30}}}};
        const auto radialFace = makeProfile(radial);
        volumeIs(revolve(radialFace, radial.plane, Axis::V, 0, 360), 9000 * std::numbers::pi,
                 "Full revolution did not produce the expected hollow cylinder");
        volumeIs(revolve(radialFace, radial.plane, Axis::V, 5, 90), 1500 * std::numbers::pi,
                 "Offset partial revolution has the wrong volume");
        volumeIs(revolve(radialFace, radial.plane, Axis::V, 10, 360), 3000 * std::numbers::pi,
                 "A side lying on the axis did not produce the expected cylinder");
        rejected([&] { revolve(radialFace, radial.plane, Axis::V, 15, 360); },
                 "An axis crossing the profile was accepted");
        rejected([&] { revolve(radialFace, radial.plane, Axis::V, 0, 361); }, "Invalid rotation was accepted");
        rejected([&] { extrude(radialFace, radial.plane, 0); }, "Zero extrusion was accepted");

        const auto a = makePrimitive(Primitive::Cube);
        const auto b = makePrimitive(Primitive::Cube, 50.0);
        const auto apart = makePrimitive(Primitive::Cube, 250.0);
        volumeIs(booleanResult(a, b, BooleanOperation::Union), 1'500'000.0, "Union has the wrong volume");
        volumeIs(booleanResult(a, b, BooleanOperation::Difference), 500'000.0, "Cut has the wrong volume");
        volumeIs(booleanResult(a, b, BooleanOperation::Intersection), 500'000.0, "Common has the wrong volume");
        volumeIs(booleanResult(a, apart, BooleanOperation::Union), 2'000'000.0, "Union lost a disconnected body");
        rejected([&] { booleanResult(a, apart, BooleanOperation::Intersection); }, "Empty intersection was accepted");
        volumeIs(a, 1'000'000.0, "Boolean modeling modified its input body");
    } catch (const Standard_Failure& error) {
        std::cerr << "OCCT: " << error.GetMessageString() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
