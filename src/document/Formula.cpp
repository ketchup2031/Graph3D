#include "document/Formula.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>

namespace graph3d::document {
namespace {
bool alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool digit(char c) { return c >= '0' && c <= '9'; }

class Parser {
public:
    Parser(std::string_view text, std::function<double(std::string)> lookup)
        : text_(text), lookup_(std::move(lookup)) {}
    double read()
    {
        if (text_.empty() || text_.size() > 512) { invalid(); }
        const double value = sum();
        spaces();
        if (!text_.empty()) { invalid(); }
        if (!std::isfinite(value)) { throw std::invalid_argument("Non-finite formula result."); }
        return value;
    }
private:
    [[noreturn]] static void invalid() { throw std::invalid_argument("Invalid formula syntax."); }
    void spaces()
    {
        while (!text_.empty() && (text_.front() == ' ' || text_.front() == '\t')) { text_.remove_prefix(1); }
    }
    bool take(char c)
    {
        spaces();
        if (text_.empty() || text_.front() != c) { return false; }
        text_.remove_prefix(1);
        return true;
    }
    double sum()
    {
        auto value = term();
        while (true) {
            if (take('+')) { value += term(); }
            else if (take('-')) { value -= term(); }
            else { return value; }
        }
    }
    double term()
    {
        auto value = unary();
        while (true) {
            if (take('*')) { value *= unary(); }
            else if (take('/')) {
                const auto divisor = unary();
                if (divisor == 0) { throw std::invalid_argument("Division by zero in formula."); }
                value /= divisor;
            } else { return value; }
        }
    }
    double unary()
    {
        if (take('+')) { return unary(); }
        if (take('-')) { return -unary(); }
        double value = primary();
        if (take('^')) { value = std::pow(value, unary()); }
        if (!std::isfinite(value)) { throw std::invalid_argument("Non-finite formula result."); }
        return value;
    }
    double primary()
    {
        if (take('(')) {
            double value = sum();
            if (!take(')')) { invalid(); }
            return value;
        }
        if (text_.empty()) { invalid(); }
        if (alpha(text_.front())) {
            auto end = std::ranges::find_if_not(text_, [](char c) { return alpha(c) || digit(c); });
            std::string name(text_.begin(), end);
            text_.remove_prefix(name.size());
            return lookup_(std::move(name));
        }
        double value = 0;
        const auto [end, error] = std::from_chars(text_.data(), text_.data() + text_.size(), value);
        if (error == std::errc::result_out_of_range) { throw std::invalid_argument("Number out of range in formula."); }
        if (error != std::errc{}) { invalid(); }
        text_.remove_prefix(static_cast<std::size_t>(end - text_.data()));
        return value;
    }
    std::string_view text_;
    std::function<double(std::string)> lookup_;
};
}

bool validVariableName(std::string_view name)
{
    return !name.empty() && name.size() <= 64 && alpha(name.front())
        && std::ranges::all_of(name, [](char c) { return alpha(c) || digit(c); });
}

double FormulaValues::evaluate(std::string_view expression) const
{
    return Parser(expression, [&](const std::string& name) {
        if (const auto it = values.find(name); it != values.end()) { return it->second; }
        if (const auto it = errors.find(name); it != errors.end()) { throw std::invalid_argument(it->second); }
        throw std::invalid_argument("Unknown variable: " + name);
    }).read();
}

FormulaValues resolveFormulas(const Configuration& configuration)
{
    auto inputs = configuration.variables;
    if (!configuration.activeVariant.empty()) {
        const auto found = configuration.variants.find(configuration.activeVariant);
        if (found == configuration.variants.end()) { throw std::invalid_argument("The selected variant does not exist."); }
        for (const auto& [name, value] : found->second) { inputs.insert_or_assign(name, value); }
    }
    FormulaValues result;
    std::set<std::string> visiting;
    std::function<double(std::string)> resolve = [&](std::string name) {
        const auto found = inputs.find(name);
        if (found == inputs.end()) { throw std::invalid_argument("Unknown variable: " + name); }
        if (result.values.contains(name)) { return result.values.at(name); }
        if (result.errors.contains(name)) { throw std::invalid_argument(result.errors.at(name)); }
        if (!visiting.insert(name).second) { throw std::invalid_argument("Cyclic variable dependency: " + name); }
        try {
            const double value = Parser(found->second, resolve).read();
            result.values.emplace(name, value);
            visiting.erase(name);
            return value;
        } catch (const std::invalid_argument& error) {
            visiting.erase(name);
            result.errors.emplace(name, error.what());
            throw;
        }
    };
    for (const auto& [name, expression] : inputs) {
        try { resolve(name); } catch (const std::invalid_argument&) {}
    }
    return result;
}
}  // namespace graph3d::document
