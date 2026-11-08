#include "markov/graph/types.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov {
namespace {

constexpr std::array<std::string_view, 3> resource_names{ "thread", "stream", "service" };
constexpr std::array<std::string_view, 6> work_names{ "host", "device", "transfer", "communication", "wait", "boundary" };
constexpr std::array<std::string_view, 7> relation_names{ "order", "submission", "data", "signal", "control", "state", "communication" };
constexpr std::array<std::string_view, 2> direction_names{ "input", "output" };
constexpr std::array<std::string_view, 4> port_use_names{ "control", "data", "metadata", "signal" };

std::int64_t integer(const nlohmann::json & value) {
    if (!value.is_number_integer()
        || (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::invalid_argument("graph metadata requires a signed 64-bit integer");
    return value.get<std::int64_t>();
}

template <class Kind, std::size_t Size> Kind read_kind(std::string_view name, const std::array<std::string_view, Size> & names) {
    const auto found = std::ranges::find(names, name);
    if (found == names.end()) throw std::invalid_argument("unknown graph kind: " + std::string(name));
    return static_cast<Kind>(found - names.begin());
}

} // namespace

ResourceKind resource_kind(std::string_view name) { return read_kind<ResourceKind>(name, resource_names); }
std::string_view resource_name(ResourceKind kind) { return resource_names.at(static_cast<std::size_t>(kind)); }
WorkKind work_kind(std::string_view name) { return read_kind<WorkKind>(name, work_names); }
std::string_view work_name(WorkKind kind) { return work_names.at(static_cast<std::size_t>(kind)); }
Relation relation_kind(std::string_view name) { return read_kind<Relation>(name, relation_names); }
std::string_view relation_name(Relation kind) { return relation_names.at(static_cast<std::size_t>(kind)); }
Direction direction_kind(std::string_view name) { return read_kind<Direction>(name, direction_names); }
std::string_view direction_name(Direction direction) { return direction_names.at(static_cast<std::size_t>(direction)); }
PortUse port_use(std::string_view name) { return read_kind<PortUse>(name, port_use_names); }
std::string_view port_use_name(PortUse use) { return port_use_names.at(static_cast<std::size_t>(use)); }

Resource Resource::read(const nlohmann::json & source) {
    Resource result{ resource_kind(source.at("kind").get<std::string>()),
                     integer(source.at("rank")),
                     source.at("role").get<std::string>(),
                     source.at("identity").get<std::string>(),
                     source.at("serial").get<bool>() };
    if (result.rank < 0 || result.role.empty() || result.identity.empty()) throw std::invalid_argument("invalid graph resource identity");
    if (result.kind != ResourceKind::Service && !result.serial) throw std::invalid_argument("threads and streams require serial order");
    return result;
}

nlohmann::json Resource::document() const {
    return {
        {     "kind", resource_name(kind) },
        {     "rank",                rank },
        {     "role",                role },
        { "identity",            identity },
        {   "serial",              serial }
    };
}

TensorObservation TensorObservation::read(const nlohmann::json & source) {
    TensorObservation result{ source.at("value").get<std::string>(),
                              source.at("storage").get<std::string>(),
                              integer(source.at("version")),
                              source.at("dtype").get<std::string>(),
                              {},
                              std::nullopt };
    for (const auto & dimension : source.at("shape").get_ref<const nlohmann::json::array_t &>()) result.shape.push_back(integer(dimension));
    if (result.value.empty() || result.storage.empty() || result.dtype.empty() || result.version < 0
        || std::ranges::any_of(result.shape, [](auto size) { return size < 0; }))
        throw std::invalid_argument("invalid observed tensor boundary");

    if (source.contains("shape_rule") && !source.at("shape_rule").is_null()) {
        const auto & rules = source.at("shape_rule");
        if (!rules.is_array() || rules.size() != result.shape.size()) throw std::invalid_argument("shape rule rank differs from observation");
        result.shape_rule.emplace();
        for (const auto & rule : rules) result.shape_rule->push_back(Expression::read(rule));
    }
    if (!source.at("writer").is_null()) {
        const auto writer = integer(source.at("writer"));
        if (writer < 0) throw std::invalid_argument("invalid tensor writer scope");
        result.writer = static_cast<std::size_t>(writer);
    }
    if (!source.at("allocation").is_null()) {
        const auto allocation = integer(source.at("allocation"));
        if (allocation < 0) throw std::invalid_argument("invalid tensor allocation scope");
        result.allocation = static_cast<std::size_t>(allocation);
    }
    result.buffer = source.at("buffer").get<std::string>();
    return result;
}

nlohmann::json TensorObservation::document() const {
    nlohmann::json result{
        {      "value",                                                              value },
        {    "storage",                                                            storage },
        {    "version",                                                            version },
        {      "dtype",                                                              dtype },
        {      "shape",                                                              shape },
        {     "writer",         writer ? nlohmann::json(*writer) : nlohmann::json(nullptr) },
        { "allocation", allocation ? nlohmann::json(*allocation) : nlohmann::json(nullptr) },
        {     "buffer", buffer }
    };
    if (shape_rule) {
        result["shape_rule"] = nlohmann::json::array();
        for (const auto & rule : *shape_rule) result["shape_rule"].push_back(rule.document());
    }
    return result;
}

ScopeInfo ScopeInfo::read(const nlohmann::json & source) {
    ScopeInfo result;
    const auto & parent = source.at("parent");
    if (!parent.is_null()) {
        const auto value = integer(parent);
        if (value < 0) throw std::invalid_argument("invalid scope parent");
        result.parent = static_cast<std::size_t>(value);
    }
    result.role = source.at("role").get<std::string>();
    result.site = source.at("site").get<std::string>();
    result.implementation = source.at("implementation").get<std::string>();
    result.parameters = read_parameters(source.at("parameters"));
    result.constants = read_parameters(source.value("constants", nlohmann::json::object()));
    result.work = source.at("work");
    result.bindings = source.value("bindings", nlohmann::json::object());
    if (result.role.empty() || !result.work.is_object() || !result.bindings.is_object()) throw std::invalid_argument("invalid scope identity or bindings");

    // Fixed implementation facts must agree with the observation, never override it.
    for (const auto & [name, value] : result.constants) {
        if (result.parameters.at(name) != value) throw std::invalid_argument("scope constant differs from observation: " + name);
    }
    const auto arguments = source.value("arguments", nlohmann::json::object());
    if (!arguments.is_object()) throw std::invalid_argument("scope arguments must be an object");
    for (const auto & [name, value] : arguments.items()) result.arguments.emplace(name, Expression::read(value));
    for (const auto & guard : source.value("guards", nlohmann::json::array())) result.guards.push_back(Expression::read(guard));
    return result;
}

nlohmann::json ScopeInfo::document() const {
    nlohmann::json arguments_json = nlohmann::json::object();
    for (const auto & [name, expression] : arguments) arguments_json[name] = expression.document();
    auto conditions = nlohmann::json::array();
    for (const auto & guard : guards) conditions.push_back(guard.document());
    return {
        {         "parent", parent ? nlohmann::json(*parent) : nlohmann::json(nullptr) },
        {           "role",                                                       role },
        {           "site",                                                       site },
        { "implementation",                                             implementation },
        {     "parameters",                                                 parameters },
        {      "constants",                                                  constants },
        {      "arguments",                                  std::move(arguments_json) },
        {           "work",                                                       work },
        {       "bindings",                                                   bindings },
        {         "guards",                                      std::move(conditions) }
    };
}

CommunicationGroup CommunicationGroup::read(const nlohmann::json & source) {
    const auto ordinal = integer(source.at("order"));
    if (ordinal < 0) throw std::invalid_argument("communication order must be nonnegative");
    CommunicationGroup result{ source.at("channel").get<std::string>(),
                               source.at("operation").get<std::string>(),
                               source.at("members").get<std::vector<std::int64_t>>(),
                               source.at("required").get<std::vector<std::int64_t>>(),
                               source.at("rank").get<std::int64_t>(),
                               static_cast<std::size_t>(ordinal) };
    result.validate();
    return result;
}

nlohmann::json CommunicationGroup::document() const {
    return {
        {   "channel",   channel },
        { "operation", operation },
        {   "members",   members },
        {  "required",  required },
        {      "rank",      rank },
        {     "order",     order }
    };
}

void CommunicationGroup::validate() const {
    const std::set<std::int64_t> participants(members.begin(), members.end());
    const std::set<std::int64_t> dependencies(required.begin(), required.end());
    if (channel.empty() || operation.empty() || participants.empty() || participants.size() != members.size() || *participants.begin() < 0
        || !participants.contains(rank) || dependencies.size() != required.size() || !std::ranges::includes(participants, dependencies))
        throw std::invalid_argument("invalid communication participant contract");
}

} // namespace markov
