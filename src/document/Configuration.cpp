#include "document/Document.h"

#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace graph3d::document {
namespace {
template<class P, class Field>
void visitFields(P& parameters, Field field)
{
    std::visit([&](auto& p) {
        using T = std::remove_cvref_t<decltype(p)>;
        if constexpr (std::is_same_v<T, geometry::Sketch>) {
            for (std::size_t i = 0; i < p.constraints.size(); ++i) {
                auto& constraint = p.constraints[i];
                using K = geometry::ConstraintKind;
                if (constraint.kind == K::Length || constraint.kind == K::Diameter || constraint.kind == K::Angle) {
                    field("dimension." + std::to_string(i + 1), constraint.value);
                }
            }
        } else if constexpr (std::is_same_v<T, Extrude>) {
            field("distance", p.distance);
        } else if constexpr (std::is_same_v<T, Revolve>) {
            if (!p.datumAxis) { field("offset", p.offset); }
            field("angle", p.angle);
        } else if constexpr (std::is_same_v<T, Hole>) {
            field("diameter", p.size.diameter);
            if (!p.size.through) { field("depth", p.size.depth); }
            if (p.size.style != geometry::HoleStyle::Straight) {
                field("headDiameter", p.size.headDiameter);
                field(p.size.style == geometry::HoleStyle::Counterbore ? "headDepth" : "headAngle", p.size.seat);
            }
            if (p.size.thread) {
                field("threadDiameter", p.size.thread->diameter);
                field("pitch", p.size.thread->pitch);
                field("threadLength", p.size.thread->length);
            }
        } else if constexpr (std::is_same_v<T, EdgeTreatment>) {
            field("size", p.size);
            if (p.endRadius) { field("endRadius", *p.endRadius); }
        } else if constexpr (std::is_same_v<T, FeaturePattern>) {
            field("count", p.count);
            if (!p.path) {
                field("stepX", p.step[0]); field("stepY", p.step[1]); field("stepZ", p.step[2]);
            }
        } else if constexpr (std::is_same_v<T, MoveFace>) {
            field("distance", p.distance);
        } else if constexpr (std::is_same_v<T, Shell>) {
            field("thickness", p.thickness);
        } else if constexpr (std::is_same_v<T, DraftAngle>) {
            field("angle", p.degrees);
        } else if constexpr (std::is_same_v<T, Transform>) {
            std::visit([&](auto& t) {
                using K = std::remove_cvref_t<decltype(t)>;
                if constexpr (std::is_same_v<K, geometry::LinearArray>) {
                    field("count", t.count);
                    field("spacing", t.spacing);
                } else if constexpr (std::is_same_v<K, geometry::PolarArray>) {
                    field("count", t.count);
                    field("angle", t.sweep);
                } else {
                    field("offset", t.offset);
                }
            }, p.parameters);
        }
    }, parameters);
}
}

std::vector<std::string> formulaFields(const Parameters& parameters)
{
    std::vector<std::string> result;
    visitFields(parameters, [&](auto name, const auto&) { result.push_back(std::move(name)); });
    return result;
}

Parameters applyBindings(const Parameters& parameters, const Expressions& bindings, const FormulaValues& values)
{
    auto result = parameters;
    std::size_t applied = 0;
    visitFields(result, [&](std::string name, auto& slot) {
        const auto it = bindings.find(name);
        if (it == bindings.end()) { return; }
        try {
            const double value = values.evaluate(it->second);
            if (std::abs(value) > 1.e9) { throw std::invalid_argument("Formula value out of range."); }
            if constexpr (std::is_same_v<std::remove_reference_t<decltype(slot)>, int>) {
                if (value != std::trunc(value)) { throw std::invalid_argument("Expected an integer formula result."); }
                slot = static_cast<int>(value);
            } else {
                slot = value;
            }
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument(name + ": " + error.what());
        }
        ++applied;
    });
    if (applied != bindings.size()) { throw std::invalid_argument("A bound dimension no longer exists."); }
    return result;
}

void validateConfiguration(const Configuration& config, const std::vector<Object>& objects)
{
    if (config.variables.size() > 128 || config.variants.size() > 64) {
        throw std::invalid_argument("Too many variables or variants.");
    }
    for (const auto& [key, value] : config.variables) {
        if (!validVariableName(key)) { throw std::invalid_argument("Invalid variable name: " + key); }
    }
    for (const auto& [label, overrides] : config.variants) {
        if (label.empty() || label.size() > 128 || label.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
            throw std::invalid_argument("Invalid variant name.");
        }
        for (const auto& [key, expression] : overrides) {
            if (!config.variables.contains(key)) { throw std::invalid_argument("Unknown variable: " + key); }
        }
    }
    for (const auto& [id, content] : config.bindings) {
        if (id < 1 || static_cast<std::size_t>(id) > objects.size() || content.empty()) {
            throw std::invalid_argument("Invalid object in formula bindings.");
        }
    }
    const auto validate = [&](const Configuration& variant) {
        const auto values = resolveFormulas(variant);
        if (!values.errors.empty()) {
            const auto& [key, error] = *values.errors.begin();
            const auto label = variant.activeVariant.empty() ? "" : variant.activeVariant + " / ";
            throw std::invalid_argument(label + key + ": " + error);
        }
        for (const auto& [id, bindings] : variant.bindings) {
            try {
                applyBindings(objects[id - 1].parameters, bindings, values);
            } catch (const std::invalid_argument& error) {
                throw std::invalid_argument("Object #" + std::to_string(id) + ": " + error.what());
            }
        }
    };
    auto variant = config;
    variant.activeVariant.clear();
    validate(variant);
    for (const auto& [label, overrides] : config.variants) {
        variant.activeVariant = label;
        validate(variant);
    }
    if (!config.activeVariant.empty() && !config.variants.contains(config.activeVariant)) {
        throw std::invalid_argument("The selected variant does not exist.");
    }
}
}  // namespace graph3d::document
