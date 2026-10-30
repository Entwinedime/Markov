#include "markov/templates/asset.hpp"

#include <algorithm>
#include <charconv>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace markov {
namespace {

void bind_role_values(FixedBindings & frame, const std::map<std::string, Expression> & rules, std::set<std::string> & unused) {
    const auto original = frame.values;
    for (auto & [name, value] : frame.values) {
        if (const auto rule = rules.find(name); rule != rules.end()) {
            value = rule->second.evaluate(original);
            unused.erase(name);
        }
    }

    for (auto & child : frame.children) bind_role_values(child, rules, unused);
    for (auto & [_, producer] : frame.producers) bind_role_values(producer, rules, unused);
}

} // namespace

TemplateAsset::TemplateAsset(const TemplateCatalog & catalog, nlohmann::json document)
    : catalog_(catalog),
      environment_(std::move(document.at("environment"))),
      physical_coverage_(std::move(document.at("physical_coverage"))),
      runtime_coverage_(std::move(document.at("runtime_coverage"))),
      execution_resources_(document.at("execution_resources").get<std::vector<std::string>>()) {
    for (const auto & source : document.at("execution_variants")) {
        ExecutionSelection selection{ source.at("definition").get<std::size_t>(), source.at("operation").get<std::size_t>(), {} };
        for (const auto & choice : source.at("choices")) {
            ExecutionVariant variant{ {}, bound_execution(choice) };
            for (const auto & guard : choice.at("guards"))
                variant.guards.push_back({ Expression::read(guard.at("condition")), guard.at("expected").get<bool>() });
            selection.choices.push_back(std::move(variant));
        }
        if (selection.choices.empty()) throw std::invalid_argument("execution selection has no observed choices");
        execution_variants_.push_back(std::move(selection));
    }

    for (const auto & source : document.at("entries")) entries_.push_back({ source.at("layout").get<std::size_t>(), fixed_bindings(source) });
    for (const auto & source : document.at("entrypoints"))
        entrypoints_.push_back({ source.at("role").get<std::string>(),
                                 source.at("phase").is_null() ? std::nullopt : std::optional(source.at("phase").get<std::string>()),
                                 source.at("entry").get<std::size_t>(),
                                 source.at("path").get<std::vector<std::size_t>>() });

    for (const auto & entry : entries_) validate_bindings(entry.layout, entry.bindings);

    // A shared catalog may include another role's families; only this role's reachable calls need its entrypoints.
    std::vector<CallVisit> visiting(catalog_.layout_count());
    for (const auto & entry : entries_) validate_calls(entry.layout, visiting);
    for (const auto & selection : entrypoints_) validate_calls(frame(selection).layout, visiting);
}

std::vector<TemplateAsset> TemplateAsset::read_roles(const TemplateCatalog & catalog, const nlohmann::json & roles) {
    std::vector<TemplateAsset> result;
    std::map<std::int64_t, std::size_t> ranks;
    for (const auto & role : roles) {
        if (role.contains("representative")) {
            const auto source = ranks.at(role.at("representative").get<std::int64_t>());
            if (result[source].environment().contains("role_source")) throw std::invalid_argument("a role representative must be an observed source");
            result.push_back(result[source].bind_role(role));
        }
        else result.emplace_back(catalog, role);

        const auto rank = result.back().environment().at("rank").get<std::int64_t>();
        if (rank < 0 || !ranks.emplace(rank, result.size() - 1).second) throw std::invalid_argument("template roles need distinct nonnegative ranks");
    }
    return result;
}

TemplateAsset TemplateAsset::bind_role(const nlohmann::json & instance) const {
    auto result = *this;
    std::map<std::string, Expression> rules;
    std::set<std::string> unused;
    for (const auto & [name, expression] : instance.at("bindings").items()) {
        rules.emplace(name, Expression::read(expression));
        unused.emplace(name);
    }

    // Rewrite declared bindings atomically in a private instance; observed calls and the catalog stay intact.
    for (auto & entry : result.entries_) bind_role_values(entry.bindings, rules, unused);
    if (!unused.empty()) throw std::invalid_argument("role mapping has no declared binding: " + *unused.begin());
    result.environment_.update(instance.at("environment"));
    result.environment_["role_source"] = {
        { "representative", instance.at("representative") },
        {       "bindings",       instance.at("bindings") }
    };
    return result;
}

BoundExecution TemplateAsset::bound_execution(const nlohmann::json & source) const {
    auto resources = source.at("resources").get<std::vector<std::size_t>>();
    for (const auto resource : resources) {
        if (resource >= execution_resources_.size()) throw std::invalid_argument("execution binding references an undefined resource");
    }

    const auto pattern = source.at("pattern").get<std::size_t>();
    const auto & execution = catalog_.execution_pattern(pattern);
    if (resources.size() != execution.sequences.size()) throw std::invalid_argument("execution resource binding arity mismatch");
    for (const auto & signal : execution.signals) {
        if (execution_resources_[resources[signal.resource]] != "event") throw std::invalid_argument("event signal requires an event resource");
    }
    for (const auto & required : execution.requirements) {
        if (required.kind == "event_wait" && execution_resources_[resources[required.resource]] != "event")
            throw std::invalid_argument("event wait requires an event resource");
    }
    BoundExecution result{ pattern, std::move(resources), std::nullopt };
    if (source.contains("reduction")) {
        result.reduction = GroupExecution::read(source.at("reduction"));
        result.reduction->validate(execution);
    }
    return result;
}

FixedBindings TemplateAsset::fixed_bindings(const nlohmann::json & source) const {
    FixedBindings result;
    result.literals = source.at("literals").get<std::vector<nlohmann::json>>();
    if (!source.at("host_resource").is_null()) {
        const auto resource = source.at("host_resource").get<std::size_t>();
        if (execution_resources_.at(resource) != "thread") throw std::invalid_argument("region host service requires a thread resource");
        result.host_resource = resource;
    }

    if (source.contains("bindings")) result.values = source.at("bindings").get<Parameters>();
    if (source.contains("execution")) {
        for (const auto & record : source.at("execution")) {
            const auto operation = record.at("operation").get<std::size_t>();
            if (record.contains("variants")) result.execution.emplace(operation, record.at("variants").get<std::size_t>());
            else result.execution.emplace(operation, bound_execution(record));
        }
    }

    for (const auto & [key, value] : source.at("external_resources").items()) {
        std::size_t resource;
        const auto parsed = std::from_chars(key.data(), key.data() + key.size(), resource);
        if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size())
            throw std::invalid_argument("external resource slot must be a nonnegative integer");
        result.external_resources.emplace(resource, value.get<std::size_t>());
    }

    for (const auto & child : source.at("children")) result.children.push_back(fixed_bindings(child));
    if (source.contains("producers")) {
        for (const auto & [name, producer] : source.at("producers").items()) result.producers.emplace(name, fixed_bindings(producer));
    }

    // Observed dynamic parameters and request effects are evidence, never target defaults.
    return result;
}

void TemplateAsset::validate_bindings(std::size_t identity, const FixedBindings & bindings) const {
    const auto & layout = catalog_.layout(identity);
    for (const auto & [operation, binding] : bindings.execution) {
        if (operation >= catalog_.definition(layout.definition).operations.size()) throw std::invalid_argument("execution operation is outside its definition");
        if (const auto * group = std::get_if<std::size_t>(&binding)) {
            const auto & selection = execution_variants_.at(*group);
            if (selection.definition != layout.definition || selection.operation != operation)
                throw std::invalid_argument("execution selection belongs to another logical operation");
        }
    }

    std::size_t child = 0;
    for (const auto & step : layout.body) {
        const auto * call = std::get_if<CallStep>(&step);
        if (!call) continue;
        const auto & frame = bindings.children.at(child++);
        validate_bindings(call->layout, frame);
        for (const auto & [name, group] : call->input_groups) {
            const auto * generated = std::get_if<GeneratedGroup>(&group);
            if (generated) {
                if (const auto * producer = std::get_if<GroupProducer>(&generated->element)) validate_bindings(producer->layout, frame.producers.at(name));
            }
        }
    }
    if (child != bindings.children.size()) throw std::invalid_argument("source child bindings do not match their layout");
}

void TemplateAsset::validate_calls(std::size_t identity, std::vector<CallVisit> & visiting) const {
    if (visiting.at(identity) == CallVisit::Complete) return;
    if (visiting[identity] == CallVisit::Active) throw std::invalid_argument("template call families contain a cycle");
    visiting[identity] = CallVisit::Active;

    const auto & source = catalog_.layout(identity);
    std::vector<std::string> roles;
    if (const auto & role = catalog_.definition(source.definition).call_role) roles.push_back(*role);
    for (const auto & step : source.body) {
        if (const auto * call = std::get_if<CallStep>(&step)) {
            if (call->family) roles.push_back(*call->family);
            else validate_calls(call->layout, visiting);
            for (const auto & [_, group] : call->input_groups) {
                if (const auto * mapping = std::get_if<MappedGroup>(&group)) roles.push_back(mapping->family);
            }
        }
    }

    // Family selection may choose a later layout; validate the possible calls rather than source numbering.
    for (const auto & role : roles) {
        bool found = false;
        for (const auto & selection : entrypoints_) {
            if (selection.role != role) continue;
            found = true;
            validate_calls(frame(selection).layout, visiting);
        }
        if (!found) throw std::invalid_argument("template call family has no entrypoint: " + role);
    }
    visiting[identity] = CallVisit::Complete;
}

const TemplateDefinition & TemplateAsset::definition(std::size_t identity) const { return catalog_.definition(identity); }
const TemplateLayout & TemplateAsset::layout(std::size_t identity) const { return catalog_.layout(identity); }
const TemplateEntry & TemplateAsset::entry(std::size_t identity) const { return entries_.at(identity); }

TemplateFrame TemplateAsset::frame(const RegionSelection & selection) const {
    const auto & root = entry(selection.entry);
    auto identity = root.layout;
    const auto * bindings = &root.bindings;
    for (const auto child : selection.path) {
        const CallStep * selected = nullptr;
        std::size_t index = 0;
        for (const auto & step : layout(identity).body) {
            if (const auto * call = std::get_if<CallStep>(&step); call != nullptr && index++ == child) {
                selected = call;
                break;
            }
        }
        if (selected == nullptr) throw std::invalid_argument("region selector references an undefined child call");

        identity = selected->layout;
        bindings = &bindings->children.at(child);
    }

    return { identity, *bindings };
}

const nlohmann::json & TemplateAsset::environment() const { return environment_; }
const nlohmann::json & TemplateAsset::physical_coverage() const { return physical_coverage_; }
const nlohmann::json & TemplateAsset::runtime_coverage() const { return runtime_coverage_; }
const std::vector<std::string> & TemplateAsset::execution_resources() const { return execution_resources_; }
const ExecutionPattern & TemplateAsset::execution_pattern(std::size_t identity) const { return catalog_.execution_pattern(identity); }

const BoundExecution & TemplateAsset::execution_binding(const ExecutionBinding & binding, const Parameters & parameters) const {
    if (const auto * fixed = std::get_if<BoundExecution>(&binding)) return *fixed;

    const auto identity = std::get<std::size_t>(binding);
    const BoundExecution * selected = nullptr;
    for (const auto & choice : execution_variants_.at(identity).choices) {
        bool matches = true;
        for (const auto & guard : choice.guards) {
            if ((guard.condition.evaluate(parameters) != 0) != guard.expected) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        if (selected) throw std::invalid_argument("multiple execution variants match target for selection " + std::to_string(identity));
        selected = &choice.binding;
    }
    if (!selected) throw LayoutMismatch("no observed execution variant matches target for selection " + std::to_string(identity));
    return *selected;
}

const std::vector<RegionSelection> & TemplateAsset::entrypoints() const { return entrypoints_; }

} // namespace markov
