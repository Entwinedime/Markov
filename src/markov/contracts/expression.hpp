#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace markov {

using Parameters = std::map<std::string, std::int64_t>;

// The same integer expression is checked on source observations and evaluated for targets.
class Expression {
public:
    enum class Kind { Add, Multiply, FloorDivide, Modulo, Minimum, Maximum, Equal, Less, LessEqual, And, Or };

    struct Operation {
        Kind kind;
        std::vector<Expression> arguments;
    };

    static Expression read(const nlohmann::json & value);

    std::int64_t evaluate(const Parameters & parameters) const;
    bool parameterized() const;
    std::set<std::string> variables() const;
    Expression substitute(const std::map<std::string, Expression> & expressions) const;
    nlohmann::json document() const;

private:
    std::variant<std::int64_t, std::string, Operation> value_;
};

// Derived names belong to the current call scope, not to its observed defaults.
class ParameterRelations {
public:
    static ParameterRelations read(const nlohmann::json & source);
    void derive(Parameters & parameters) const;
    const std::vector<std::pair<std::string, Expression>> & expressions() const;

private:
    std::vector<std::pair<std::string, Expression>> expressions_;
};

} // namespace markov
