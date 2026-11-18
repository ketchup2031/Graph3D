#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace graph3d::document {

using Expressions = std::map<std::string, std::string>;

struct Configuration {
    Expressions variables;
    std::map<std::string, Expressions> variants;
    std::string activeVariant;
    std::map<int, Expressions> bindings;
    bool operator==(const Configuration&) const = default;
};

struct FormulaValues {
    std::map<std::string, double> values;
    std::map<std::string, std::string> errors;
    double evaluate(std::string_view expression) const;
};

bool validVariableName(std::string_view name);
FormulaValues resolveFormulas(const Configuration& configuration);

}  // namespace graph3d::document
