#include "markov/contracts/expression.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace markov {
namespace {

const std::map<std::string, Expression::Kind> operations{
    {       "add",         Expression::Kind::Add },
    {       "mul",    Expression::Kind::Multiply },
    { "floor_div", Expression::Kind::FloorDivide },
    {       "mod",      Expression::Kind::Modulo },
    {       "min",     Expression::Kind::Minimum },
    {       "max",     Expression::Kind::Maximum },
    {        "eq",       Expression::Kind::Equal },
    {        "lt",        Expression::Kind::Less },
    {        "le",   Expression::Kind::LessEqual },
    {       "and",         Expression::Kind::And },
    {        "or",          Expression::Kind::Or },
};

std::int64_t add(std::int64_t left, std::int64_t right) {
    std::int64_t result;
    if (__builtin_add_overflow(left, right, &result)) throw std::overflow_error("shape addition overflow");

    return result;
}

std::int64_t multiply(std::int64_t left, std::int64_t right) {
    std::int64_t result;
    if (__builtin_mul_overflow(left, right, &result)) throw std::overflow_error("shape multiplication overflow");

    return result;
}

std::int64_t floor_divide(std::int64_t numerator, std::int64_t denominator) {
    if (denominator == 0 || (numerator == std::numeric_limits<std::int64_t>::min() && denominator == -1)) throw std::domain_error("invalid shape division");

    // Python-style shape arithmetic floors negative quotients instead of truncating them.
    auto quotient = numerator / denominator;
    if (numerator % denominator != 0 && (numerator < 0) != (denominator < 0)) --quotient;

    return quotient;
}

} // namespace

Expression Expression::read(const nlohmann::json & value) {
    Expression result;
    if (value.is_number_integer()) result.value_ = value.get<std::int64_t>();
    else if (value.is_string()) result.value_ = value.get<std::string>();
    else {
        const auto kind = operations.at(value.at(0).get<std::string>());
        const auto count = value.size() - 1;

        if (count == 0
            || ((kind == Kind::FloorDivide || kind == Kind::Modulo || kind == Kind::Equal || kind == Kind::Less || kind == Kind::LessEqual) && count != 2))
            throw std::invalid_argument("invalid expression arity");

        Operation operation{ kind, {} };
        for (std::size_t index = 1; index < value.size(); ++index) operation.arguments.push_back(read(value.at(index)));

        result.value_ = std::move(operation);
    }

    return result;
}

std::set<std::string> Expression::variables() const {
    if (const auto * name = std::get_if<std::string>(&value_)) return { *name };

    std::set<std::string> result;
    if (const auto * operation = std::get_if<Operation>(&value_)) {
        for (const auto & argument : operation->arguments) result.merge(argument.variables());
    }
    return result;
}

std::int64_t Expression::evaluate(const Parameters & parameters) const {
    if (const auto * constant = std::get_if<std::int64_t>(&value_)) return *constant;

    if (const auto * parameter = std::get_if<std::string>(&value_)) {
        const auto found = parameters.find(*parameter);
        if (found == parameters.end()) throw std::invalid_argument("missing parameter: " + *parameter);

        return found->second;
    }

    const auto & operation = std::get<Operation>(value_);
    auto result = operation.arguments.front().evaluate(parameters);

    for (std::size_t index = 1; index < operation.arguments.size(); ++index) {
        const auto next = operation.arguments[index].evaluate(parameters);

        switch (operation.kind) {
        case Kind::Add:
            result = add(result, next);
            break;
        case Kind::Multiply:
            result = multiply(result, next);
            break;

        case Kind::FloorDivide:
            result = floor_divide(result, next);
            break;

        case Kind::Modulo: {
            floor_divide(result, next);
            result %= next;
            if (result != 0 && (result < 0) != (next < 0)) result = add(result, next);
            break;
        }

        case Kind::Minimum:
            result = std::min(result, next);
            break;
        case Kind::Maximum:
            result = std::max(result, next);
            break;
        case Kind::Equal:
            result = result == next;
            break;
        case Kind::Less:
            result = result < next;
            break;
        case Kind::LessEqual:
            result = result <= next;
            break;
        case Kind::And:
            result = result != 0 && next != 0;
            break;
        case Kind::Or:
            result = result != 0 || next != 0;
            break;
        }
    }

    return result;
}

nlohmann::json Expression::document() const {
    if (const auto * constant = std::get_if<std::int64_t>(&value_)) return *constant;
    if (const auto * parameter = std::get_if<std::string>(&value_)) return *parameter;

    const auto & operation = std::get<Operation>(value_);
    nlohmann::json result = nlohmann::json::array();
    for (const auto & [name, kind] : operations) {
        if (kind == operation.kind) {
            result.push_back(name);
            break;
        }
    }

    for (const auto & argument : operation.arguments) result.push_back(argument.document());

    // Binary commutation preserves checked arithmetic; reordering longer sums/products can change overflow.
    if (operation.arguments.size() == 2 && (operation.kind == Kind::Add || operation.kind == Kind::Multiply || operation.kind == Kind::Equal)
        && result[2] < result[1])
        std::swap(result[1], result[2]);

    return result;
}

Expression Expression::substitute(const std::map<std::string, Expression> & expressions) const {
    if (const auto * name = std::get_if<std::string>(&value_)) {
        const auto found = expressions.find(*name);
        return found == expressions.end() ? *this : found->second;
    }

    auto result = *this;
    if (auto * operation = std::get_if<Operation>(&result.value_)) {
        for (auto & argument : operation->arguments) argument = argument.substitute(expressions);
    }
    return result;
}

bool Expression::parameterized() const {
    if (std::holds_alternative<std::string>(value_)) return true;

    if (const auto * operation = std::get_if<Operation>(&value_)) {
        for (const auto & argument : operation->arguments) {
            if (argument.parameterized()) return true;
        }
    }

    return false;
}

ParameterRelations ParameterRelations::read(const nlohmann::json & source) {
    ParameterRelations result;
    std::set<std::string> outputs;

    for (const auto & record : source) {
        auto name = record.at("name").get<std::string>();
        if (!outputs.insert(name).second) throw std::invalid_argument("duplicate derived parameter: " + name);

        result.expressions_.emplace_back(std::move(name), Expression::read(record.at("expression")));
    }

    return result;
}

void ParameterRelations::derive(Parameters & parameters) const {

    // Outputs belong to this scope; observed or inherited outputs cannot supply missing inputs.
    for (const auto & [name, _] : expressions_) parameters.erase(name);

    for (const auto & [name, expression] : expressions_) parameters.emplace(name, expression.evaluate(parameters));
}

const std::vector<std::pair<std::string, Expression>> & ParameterRelations::expressions() const { return expressions_; }

} // namespace markov
