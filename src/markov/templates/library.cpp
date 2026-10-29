#include "markov/templates/library.hpp"

#include "markov/contracts/input_group.hpp"
#include "markov/production/execution_source.hpp"
#include "markov/templates/group_execution.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {
namespace {

void append_once(std::vector<std::size_t> & values, std::size_t value) {
    if (std::ranges::find(values, value) == values.end()) values.push_back(value);
}

void canonicalize_expression(nlohmann::json & expression) {
    if (!expression.is_array()) return;
    for (auto argument = std::next(expression.begin()); argument != expression.end(); ++argument) canonicalize_expression(*argument);

    static const std::set<std::string> commutative{ "add", "mul", "min", "max", "eq", "and", "or" };
    if (commutative.contains(expression.at(0).get<std::string>())) std::sort(std::next(expression.begin()), expression.end());
}

nlohmann::json execution_signature(const nlohmann::json & frame) {
    nlohmann::json result{
        { "host_resource", frame.at("host_resource") },
        { "operations", frame.value("execution", nlohmann::json::array()) }
    };

    std::map<std::pair<std::int64_t, std::int64_t>, std::size_t> queue_items;
    std::map<std::pair<std::string, std::int64_t>, std::map<std::size_t, std::vector<nlohmann::json *>>> orders;
    for (auto & operation : result["operations"]) {
        operation.erase("candidate");
        auto & host = operation.at("observation").at("host_scope");
        if (!host.is_null()) {
            host.erase("source_event");
            host.erase("identity");
            for (const auto * field : { "order", "completion_order" }) {
                auto & order = host.at(field);
                orders[{ "thread", host.at("thread").get<std::int64_t>() }][order.get<std::size_t>()].push_back(&order);
            }
        }

        for (const auto * kind : { "device_work", "runtime_actions" }) {
            for (auto & effect : operation.at("observation").at(kind)) {

                // Compare observed behavior and resource identity, not capture-local record numbers.
                for (const auto * key : { "source_event", "operation", "region", "baseline", "observer", "auxiliary" }) effect.erase(key);

                auto & submission = effect.at("submission");
                if (!submission.is_null()) {
                    const auto thread = submission.at("thread").get<std::int64_t>();
                    auto & order = submission.at("order");
                    orders[{ "thread", thread }][order.get<std::size_t>()].push_back(&order);
                    auto & completion = submission.at("completion_order");
                    orders[{ "thread", thread }][completion.get<std::size_t>()].push_back(&completion);
                    submission.erase("source_event");

                    auto & queue = submission.at("queue");
                    if (!queue.is_null()) {
                        const auto key = std::pair{ queue.at("process").get<std::int64_t>(), queue.at("correlation").get<std::int64_t>() };
                        queue["correlation"] = queue_items.try_emplace(key, queue_items.size()).first->second;

                        auto & enqueue_order = queue.at("enqueue_order");
                        auto & dequeue_order = queue.at("dequeue_order");
                        auto & completion_order = queue.at("dequeue_completion_order");
                        orders[{ "thread", queue.at("producer_thread").get<std::int64_t>() }][enqueue_order.get<std::size_t>()].push_back(&enqueue_order);
                        orders[{ "thread", thread }][dequeue_order.get<std::size_t>()].push_back(&dequeue_order);
                        orders[{ "thread", thread }][completion_order.get<std::size_t>()].push_back(&completion_order);

                        queue.erase("enqueue_event");
                        queue.erase("dequeue_event");
                    }
                }

                if (effect.contains("order")) {
                    auto & order = effect.at("order");
                    orders[{ "stream", effect.at("stream").get<std::int64_t>() }][order.get<std::size_t>()].push_back(&order);
                }
                if (effect.contains("native_call") && !effect.at("native_call").is_null()) effect["native_call"].erase("source");
            }
        }
    }

    // Ordering is meaningful within a serial resource; elapsed gaps and cross-resource timing are not edges.
    for (const auto & [_, positions] : orders) {
        std::size_t order = 0;
        for (const auto & [source_order, records] : positions) {
            for (auto * record : records) *record = order;
            ++order;
        }
    }

    return result;
}

// Source identities disappear from shared definitions through encounter-ordered local slots.
class LocalValues {
public:
    LocalValues(const std::map<std::size_t, const nlohmann::json *> & source, const std::map<std::size_t, nlohmann::json> & declarations,
                const std::set<std::size_t> & grouped)
        : source_(source),
          declarations_(declarations),
          grouped_(grouped) {}

    std::size_t resource(std::size_t source) {
        const auto [position, added] = resources_.try_emplace(source, resources_.size());
        if (added) resources.push_back(source);
        return position->second;
    }

    std::size_t value(std::size_t source) {

        // A fixed slot would retain source arity instead of binding the target collection.
        if (grouped_.contains(source)) throw std::invalid_argument("input group member requires a group binding");

        const auto [position, added] = values_.try_emplace(source, values_.size());
        if (added) {
            const auto & record = *source_.at(source);
            values.push_back({
                {    "kind",                                                                                "tensor" },
                { "storage",                                       resource(record.at("storage").get<std::size_t>()) },
                {   "shape", declarations_.contains(source) ? declarations_.at(source) : record.at("shape_relation") },
                {   "dtype",                                                                      record.at("dtype") },
                {  "device",                                                                     record.at("device") }
            });
        }

        return position->second;
    }

    void opaque(std::size_t source) {
        auto & record = values[value(source)];
        record["kind"] = "resource";
        record.erase("shape");
    }

    nlohmann::json ports(const std::vector<std::size_t> & source) {
        auto result = nlohmann::json::array();
        for (const auto identity : source) result.push_back(value(identity));
        return result;
    }

    nlohmann::json arguments(const nlohmann::json & source) {
        if (source.is_object() && source.size() == 1) {
            if (source.contains("value"))
                return {
                    { "value", value(source.at("value").get<std::size_t>()) }
                };
            if (source.contains("shape") || source.contains("scalar") || source.contains("input_group")) return source;
        }

        // Fixed numeric operands belong to this call; geometry expressions retain their declared relations.
        if (source.is_number()) {
            const auto identity = literals.size();
            literals.push_back(source);
            return {
                { "literal", identity }
            };
        }

        auto result = source;
        if (result.is_structured()) {
            for (auto & item : result) item = arguments(item);
        }

        return result;
    }

    nlohmann::json bind_values(LocalValues & target) const {
        std::vector<std::size_t> bindings(values_.size());
        for (const auto & [source, local] : values_) bindings[local] = target.value(source);
        return bindings;
    }

    void require_geometry() const {
        for (const auto & [source, local] : values_) {
            const auto & record = values[local];
            if (record.at("kind") == "tensor" && record.at("shape").is_null())
                throw std::invalid_argument("tensor shape relation unavailable for source value " + std::to_string(source));
        }
    }

    nlohmann::json values = nlohmann::json::array();
    std::vector<nlohmann::json> literals;
    std::vector<std::size_t> resources;

private:
    const std::map<std::size_t, const nlohmann::json *> & source_;
    const std::map<std::size_t, nlohmann::json> & declarations_;
    const std::set<std::size_t> & grouped_;
    std::map<std::size_t, std::size_t> values_;
    std::map<std::size_t, std::size_t> resources_;
};

} // namespace

// Compile bottom-up so parent layouts reuse already-normalized child definitions.
class SourceTemplates {
public:
    SourceTemplates(const nlohmann::json & source, TemplateLibrary & library);
    nlohmann::json document();

private:
    struct Region {
        std::vector<std::size_t> children;
        std::vector<std::size_t> operations;

        std::vector<std::size_t> inputs;
        std::vector<std::size_t> outputs;
        std::vector<std::size_t> resources;

        std::size_t layout = 0;
        nlohmann::json frame;

        std::map<std::size_t, nlohmann::json> declarations;
        GroupValues input_groups;
        std::map<std::size_t, std::size_t> call_indices;
    };

    struct Item {
        std::size_t position;
        bool operation;
        std::size_t identity;
    };

    struct Repetition {
        std::string kind;
        nlohmann::json count;
        std::vector<std::size_t> members;
        std::vector<std::size_t> producers;
    };

    void index();
    void compile(std::size_t region);
    std::pair<nlohmann::json, std::vector<std::size_t>> execution_selections() const;
    static void publish_selections(nlohmann::json & frame, std::span<const std::size_t> groups);
    std::optional<std::size_t> host_resource(std::size_t region, const std::vector<Item> & items);
    std::map<std::size_t, std::map<std::string, Repetition>> repetitions(const std::vector<Item> & items) const;

    bool contains(std::size_t region, std::size_t operation) const;
    bool contains_region(std::size_t region, std::size_t child) const;
    void use(std::size_t region, std::size_t value);

    // The producer owns this immutable source throughout library compilation.
    const nlohmann::json & source_;
    TemplateLibrary & library_;
    ExecutionSource execution_;

    std::set<std::size_t> external_resources_;
    std::map<std::size_t, const nlohmann::json *> values_;
    std::map<std::string, const nlohmann::json *> modules_;
    std::vector<Region> regions_;
    std::vector<std::size_t> roots_;

    std::vector<nlohmann::json> execution_candidates_;
    std::set<std::size_t> external_events_;
};


SourceTemplates::SourceTemplates(const nlohmann::json & source, TemplateLibrary & library)
    : source_(source),
      library_(library),
      external_resources_(source.at("external_resources").get<std::set<std::size_t>>()),
      regions_(source.at("regions").size()) {
    index();
    for (const auto root : roots_) compile(root);
}

bool SourceTemplates::contains(std::size_t region, std::size_t operation) const {
    return contains_region(region, source_.at("operations").at(operation).at("region").get<std::size_t>());
}

bool SourceTemplates::contains_region(std::size_t region, std::size_t child) const {
    while (child != region) {
        const auto & parent = source_.at("regions").at(child).at("parent");
        if (parent.is_null()) return false;
        child = parent.get<std::size_t>();
    }

    return true;
}

void SourceTemplates::use(std::size_t region, std::size_t value) {
    const auto & producer = values_.at(value)->at("producer");
    auto owner = region;

    // Propagate a crossing value up both sides until producer and consumer share a scope.
    while (true) {
        if (producer.is_null() || !contains(owner, producer.get<std::size_t>())) append_once(regions_[owner].inputs, value);
        const auto & parent = source_.at("regions")[owner].at("parent");
        if (parent.is_null()) break;
        owner = parent.get<std::size_t>();
    }

    if (producer.is_null()) return;
    owner = source_.at("operations").at(producer.get<std::size_t>()).at("region").get<std::size_t>();
    while (!contains_region(owner, region)) {
        append_once(regions_[owner].outputs, value);
        const auto & parent = source_.at("regions")[owner].at("parent");
        if (parent.is_null()) break;
        owner = parent.get<std::size_t>();
    }
}

void SourceTemplates::index() {
    for (const auto & value : source_.at("values")) values_.emplace(value.at("id").get<std::size_t>(), &value);
    for (const auto & module : source_.at("modules")) modules_.emplace(module.at("path").get<std::string>(), &module);

    for (std::size_t identity = 0; identity < regions_.size(); ++identity) {
        const auto & region = source_.at("regions")[identity];
        const auto & owner = region.at("interface_owner");
        if (!owner.is_null() && owner.get<std::size_t>() != identity) continue;
        if (region.at("parent").is_null()) roots_.push_back(identity);
        else regions_[region.at("parent").get<std::size_t>()].children.push_back(identity);

        const auto & metadata = region.at("metadata");
        if (metadata.contains("ports")) {
            for (const auto & [_, value] : metadata.at("ports").at("inputs").items()) use(identity, value.get<std::size_t>());
            for (const auto & [_, value] : metadata.at("ports").at("outputs").items()) {
                use(identity, value.get<std::size_t>());
                append_once(regions_[identity].outputs, value.get<std::size_t>());
            }
        }

        if (metadata.contains("input_groups")) {
            for (const auto & [name, group] : metadata.at("input_groups").items()) {
                for (const auto & member : group.at("members")) regions_[identity].input_groups[name].push_back(member.at("value").get<std::size_t>());
            }
        }

        if (!region.at("parent").is_null()) regions_[identity].declarations = regions_[region.at("parent").get<std::size_t>()].declarations;
        if (metadata.contains("shape_contracts")) {
            for (const auto & contract : metadata.at("shape_contracts")) {
                const auto value = contract.at("value").get<std::size_t>();
                regions_[identity].declarations.insert_or_assign(value, contract.at("shape"));
                use(identity, value);
            }
        }

        if (metadata.contains("inputs")) {
            for (const auto & value : metadata.at("inputs")) use(identity, value.get<std::size_t>());
        }
        if (metadata.contains("source")) use(identity, metadata.at("source").get<std::size_t>());

        for (const auto & key : { "outputs", "mutated_values" }) {
            if (metadata.contains(key)) {
                for (const auto & value : metadata.at(key)) append_once(regions_[identity].outputs, value.get<std::size_t>());
            }
        }
    }

    // Infer ports from actual producers and consumers, including state hidden behind native objects.
    for (const auto & operation : source_.at("operations")) {
        const auto identity = operation.at("id").get<std::size_t>();
        const auto region = operation.at("region").get<std::size_t>();
        regions_[region].operations.push_back(identity);
        for (const auto & value : operation.at("inputs")) use(region, value.get<std::size_t>());
    }

    for (auto & region : regions_) {
        for (const auto & [_, members] : region.input_groups) {
            std::erase_if(region.inputs, [&](auto value) { return std::ranges::find(members, value) != members.end(); });
        }
    }
}

std::map<std::size_t, std::map<std::string, SourceTemplates::Repetition>> SourceTemplates::repetitions(const std::vector<Item> & items) const {
    std::map<std::size_t, std::map<std::string, Repetition>> result;
    for (std::size_t position = 0; position < items.size(); ++position) {
        const auto & item = items[position];
        if (item.operation) continue;
        const auto & metadata = source_.at("regions")[item.identity].at("metadata");
        if (!metadata.contains("group_repetitions")) continue;

        for (const auto & [name, declaration] : metadata.at("group_repetitions").items()) {
            Repetition repetition{ declaration.at("kind").get<std::string>(), declaration.at("count"), regions_[item.identity].input_groups.at(name), {} };
            if (repetition.members.empty()) throw std::invalid_argument("a repeated group needs a source element");

            if (repetition.kind == "reuse") {
                if (!std::ranges::all_of(repetition.members, [&](auto value) { return value == repetition.members.front(); }))
                    throw std::invalid_argument("reused group members must reference the same source value");
            }
            else if (repetition.kind == "produce") {
                if (position < repetition.members.size()) throw std::invalid_argument("group producers must immediately precede their consumer");
                for (std::size_t index = 0; index < repetition.members.size(); ++index) {
                    const auto value = repetition.members[index];
                    const auto & producer = values_.at(value)->at("producer");
                    if (producer.is_null()) throw std::invalid_argument("produced group member has no source producer");

                    const auto region_id = source_.at("operations").at(producer.get<std::size_t>()).at("region").get<std::size_t>();
                    const auto & preceding = items[position - repetition.members.size() + index];
                    if (preceding.operation || preceding.identity != region_id)
                        throw std::invalid_argument("group producers must immediately precede their consumer");

                    const auto & region = regions_[region_id];
                    const auto & producer_record = source_.at("regions")[region_id];
                    const auto & producer_metadata = producer_record.at("metadata");
                    if (producer_record.at("role") != "operation" || !region.children.empty() || !region.inputs.empty()
                        || region.outputs != std::vector<std::size_t>{ value } || producer_metadata.contains("entrypoint")
                        || producer_metadata.contains("state_effects")
                        || (producer_metadata.contains("parameters") && !producer_metadata.at("parameters").empty()))
                        throw std::invalid_argument("group producer must be a leaf with one output and no external inputs or state effects");

                    // Repetition replaces copies only when their structure and observed effects agree.
                    if (!repetition.producers.empty()) {
                        const auto & prototype = regions_[repetition.producers.front()];
                        if (region.layout != prototype.layout
                            || region.frame.value("bindings", nlohmann::json::object()) != prototype.frame.value("bindings", nlohmann::json::object())
                            || region.frame.at("literals") != prototype.frame.at("literals"))
                            throw std::invalid_argument("group element producers do not share one template and fixed bindings");
                        if (execution_signature(region.frame) != execution_signature(prototype.frame))
                            throw std::invalid_argument("group element producers have different observed execution effects");
                    }

                    for (const auto & operation : source_.at("operations")) {
                        const auto & inputs = operation.at("inputs");
                        if (std::find(inputs.begin(), inputs.end(), value) != inputs.end() && !contains(item.identity, operation.at("id").get<std::size_t>()))
                            throw std::invalid_argument("generated group member escapes its consumer");
                    }

                    repetition.producers.push_back(region_id);
                }
            }
            else throw std::invalid_argument("unknown group repetition kind");
            result[item.identity].emplace(name, std::move(repetition));
        }
    }

    return result;
}

std::optional<std::size_t> SourceTemplates::host_resource(std::size_t identity, const std::vector<Item> & items) {
    const auto & record = source_.at("regions")[identity];
    const auto & scope = record.at("host_scope");

    // A native interface's operation already owns its complete caller service.
    if (scope.is_null() || !record.at("interface_owner").is_null()) return std::nullopt;

    auto cursor = scope.at("order").get<std::size_t>();
    for (const auto & item : items) {
        const auto & child = item.operation ? source_.at("operations")[item.identity].at("execution_observation").at("host_scope")
                                            : source_.at("regions")[item.identity].at("host_scope");
        if (child.is_null() || child.at("thread") != scope.at("thread") || child.at("order").get<std::size_t>() < cursor
            || child.at("completion_order") > scope.at("completion_order"))
            throw std::invalid_argument("region host children must be ordered, nested calls on its caller thread");
        cursor = child.at("completion_order").get<std::size_t>();
    }

    return execution_.resource("thread", scope.at("thread").get<std::int64_t>());
}

void SourceTemplates::compile(std::size_t identity) {
    auto & region = regions_[identity];
    const auto & record = source_.at("regions")[identity];
    const auto & metadata = record.at("metadata");

    // Child layouts must be available before this caller can bind and deduplicate its composition.
    for (const auto child : region.children) compile(child);

    std::vector<Item> items;
    for (const auto child : region.children) items.push_back({ source_.at("regions")[child].at("operation_begin").get<std::size_t>(), false, child });

    const bool dynamic_calls = metadata.value("dynamic_calls", false);
    std::set<std::size_t> exposed;
    if (dynamic_calls && !region.operations.empty()) {
        exposed.insert(region.outputs.begin(), region.outputs.end());
        for (const auto child : region.children) {
            exposed.insert(regions_[child].inputs.begin(), regions_[child].inputs.end());
            for (const auto & [_, values] : regions_[child].input_groups) exposed.insert(values.begin(), values.end());
        }
    }

    for (const auto operation_id : region.operations) {
        const auto & operation = source_.at("operations")[operation_id];
        if (!dynamic_calls) {
            items.push_back({ operation.at("position").get<std::size_t>(), true, operation_id });
            continue;
        }

        // Private control calculations belong to caller CPU service; crossing data and device work stay explicit.
        const auto & observation = operation.at("execution_observation");
        const auto cpu_values = [this](const nlohmann::json & values) {
            return std::ranges::all_of(values, [this](const auto & value) { return values_.at(value.template get<std::size_t>())->at("device") == "cpu"; });
        };
        const auto escapes =
            std::ranges::any_of(operation.at("outputs"), [&](const auto & value) { return exposed.contains(value.template get<std::size_t>()); });
        const auto writes_state =
            std::ranges::any_of(operation.at("writes"), [&](const auto & value) { return external_resources_.contains(value.template get<std::size_t>()); });
        if (!cpu_values(operation.at("inputs")) || !cpu_values(operation.at("outputs")) || escapes || writes_state || !observation.at("device_work").empty()
            || !observation.at("runtime_actions").empty() || !observation.at("communication").is_null())
            throw std::invalid_argument("dynamic caller exposes operation data or execution effects: " + record.at("role").get<std::string>());
    }

    std::ranges::sort(items, [](const auto & left, const auto & right) {
        return std::tie(left.position, left.operation, left.identity) < std::tie(right.position, right.operation, right.identity);
    });

    const auto repetitions = this->repetitions(items);
    std::set<std::size_t> producers;
    std::set<std::size_t> produced_values;
    for (const auto & [_, groups] : repetitions) {
        for (const auto & [group_name, repetition] : groups) {
            if (repetition.kind != "produce") continue;
            for (const auto producer : repetition.producers) {
                if (!producers.insert(producer).second) throw std::invalid_argument("one producer cannot generate two independent groups");
            }
            produced_values.insert(repetition.members.begin(), repetition.members.end());
        }
    }

    auto external_declarations = region.declarations;
    std::erase_if(external_declarations, [&](const auto & item) {
        const auto & producer = values_.at(item.first)->at("producer");
        return !producer.is_null() && contains(identity, producer.template get<std::size_t>());
    });

    std::set<std::size_t> grouped_values;
    for (const auto & [_, members] : region.input_groups) grouped_values.insert(members.begin(), members.end());
    grouped_values.insert(produced_values.begin(), produced_values.end());

    LocalValues local(values_, external_declarations, grouped_values);
    LocalValues own(values_, region.declarations, grouped_values);
    nlohmann::json definition{
        { "role",       record.at("role") },
        { "body", nlohmann::json::array() }
    };
    nlohmann::json layout{
        {  "inputs",  local.ports(region.inputs) },
        { "outputs", local.ports(region.outputs) },
        {    "body",     nlohmann::json::array() }
    };
    layout["ports"] = {
        {  "inputs", nlohmann::json::object() },
        { "outputs", nlohmann::json::object() }
    };
    if (metadata.contains("ports")) {
        for (const auto & [direction, ports] : metadata.at("ports").items()) {
            for (const auto & [name, value] : ports.items()) layout["ports"][direction][name] = local.value(value.get<std::size_t>());
        }
    }

    // Public port contracts constrain target geometry independently of private operation values.
    layout["input_shapes"] = nlohmann::json::array();
    std::map<std::size_t, nlohmann::json> port_shapes;
    std::set<std::size_t> opaque_inputs;
    if (metadata.contains("opaque_inputs")) opaque_inputs = metadata.at("opaque_inputs").get<std::set<std::size_t>>();
    if (metadata.contains("ports")) {
        for (const auto & [_, port] : metadata.at("ports").at("inputs").items()) {
            const auto value = port.get<std::size_t>();
            if (opaque_inputs.contains(value) || grouped_values.contains(value)) continue;
            const auto & shape = local.values.at(local.value(value)).at("shape");
            if (!shape.is_null()) port_shapes.emplace(value, shape);
        }
    }
    if (metadata.contains("inputs")) {
        for (const auto & input : metadata.at("inputs")) {
            const auto value = input.get<std::size_t>();
            if (opaque_inputs.contains(value) || grouped_values.contains(value)) continue;
            port_shapes.emplace(value, local.values.at(local.value(value)).at("shape"));
        }
    }

    if (metadata.contains("source")) {
        const auto value = metadata.at("source").get<std::size_t>();
        port_shapes.emplace(value, local.values.at(local.value(value)).at("shape"));
    }

    if (metadata.contains("shape_contracts")) {
        for (const auto & contract : metadata.at("shape_contracts"))
            port_shapes.insert_or_assign(contract.at("value").get<std::size_t>(), contract.at("shape"));
    }

    for (const auto & [value, shape] : port_shapes) {
        if (shape.is_null()) throw std::invalid_argument("public input port has no geometry");
        const auto & type = *values_.at(value);
        layout["input_shapes"].push_back({
            {  "value", local.value(value) },
            {  "shape",              shape },
            {  "dtype",   type.at("dtype") },
            { "device",  type.at("device") }
        });
    }

    if (record.at("role") == "module") definition["type"] = modules_.at(metadata.at("path").get<std::string>())->at("type");
    if (record.at("role") == "call") definition["type"] = metadata.at("path");

    std::set<std::string> derived;
    if (metadata.contains("input_groups")) {
        definition["input_groups"] = nlohmann::json::object();
        for (const auto & [name, specification] : metadata.at("input_groups").items()) {
            auto group = specification;
            group.erase("members");
            derived.insert(group.at("count").get<std::string>());
            for (const auto & [parameter, _] : group.at("sums").items()) derived.insert(parameter);
            definition["input_groups"][name] = std::move(group);
        }
    }

    for (const auto & key : { "guards", "derived_parameters", "state_effects", "dynamic_calls", "call_role", "call_count" }) {
        if (metadata.contains(key)) definition[key] = metadata.at(key);
    }
    if (metadata.contains("derived_parameters")) {
        for (const auto & relation : metadata.at("derived_parameters")) derived.insert(relation.at("name").get<std::string>());
    }

    region.frame = nlohmann::json::object();
    const auto host = host_resource(identity, items);
    region.frame["host_resource"] = host ? nlohmann::json(*host) : nlohmann::json(nullptr);
    for (const auto & key : { "bindings", "parameters", "effects", "requests", "generated_tokens", "input_groups" }) {
        if (metadata.contains(key)) region.frame[key] = metadata.at(key);
    }

    for (const auto & key : { "bindings", "parameters" }) {
        if (!metadata.contains(key)) continue;

        definition[key] = nlohmann::json::array();
        for (const auto & [name, _] : metadata.at(key).items()) {
            if (!derived.contains(name)) definition[key].push_back(name);
        }
    }

    region.frame["children"] = nlohmann::json::array();
    for (const auto & item : items) {
        if (!item.operation && producers.contains(item.identity)) continue;

        nlohmann::json body;
        if (item.operation) {
            const auto & operation = source_.at("operations")[item.identity];
            nlohmann::json work;
            if (operation.contains("execution_observation")) {
                auto prototype = execution_.compile(operation.at("execution_observation"));
                std::optional<GroupExecution> reduction;
                if (operation.contains("execution_reduction")) {
                    const auto & declaration = operation.at("execution_reduction");
                    const auto & group = operation.at("input_groups").at(declaration.at("group").get<std::string>());
                    std::vector<Parameters> members;
                    for (const auto & member : group.at("members")) members.push_back(member.at("parameters").get<Parameters>());
                    reduction = GroupExecution::compile(declaration, prototype.pattern, members);
                }
                for (const auto & requirement : prototype.pattern.at("requirements")) {
                    if (requirement.at("kind") == "event_wait") external_events_.insert(prototype.resources.at(requirement.at("resource").get<std::size_t>()));
                }
                const auto pattern = library_.execution_patterns_.try_emplace(std::move(prototype.pattern), library_.execution_patterns_.size()).first->second;
                if (!region.frame.contains("execution")) region.frame["execution"] = nlohmann::json::array();
                region.frame["execution"].push_back({
                    {   "operation",          definition.at("body").size() },
                    {     "pattern",                               pattern },
                    {   "resources",        std::move(prototype.resources) },
                    { "observation", operation.at("execution_observation") }
                });
                if (operation.contains("execution_guards")) region.frame["execution"].back()["guards"] = operation.at("execution_guards");
                if (reduction) region.frame["execution"].back()["reduction"] = reduction->document();
            }

            for (const auto & key : { "opaque_inputs", "opaque_outputs" }) {
                if (!operation.contains(key)) continue;
                for (const auto & value : operation.at(key)) {
                    if (!grouped_values.contains(value.get<std::size_t>())) own.opaque(value.get<std::size_t>());
                }
            }

            for (const auto & key : { "kind", "name", "attributes" }) {
                if (operation.contains(key)) work[key] = operation.at(key);
            }

            auto inputs = operation.at("inputs").get<std::vector<std::size_t>>();
            if (operation.contains("input_groups")) std::erase_if(inputs, [&](auto value) { return grouped_values.contains(value); });
            work["inputs"] = own.ports(inputs);
            work["outputs"] = own.ports(operation.at("outputs").get<std::vector<std::size_t>>());
            if (operation.contains("input_groups")) {
                work["input_groups"] = nlohmann::json::array();
                for (const auto & [name, _] : operation.at("input_groups").items()) work["input_groups"].push_back(name);
            }
            for (const auto & key : { "arguments", "keywords" }) work[key] = own.arguments(operation.at(key));
            if (operation.contains("communication")) work["communication"] = own.arguments(operation.at("communication"));
            work["writes"] = nlohmann::json::array();
            for (const auto & storage : operation.at("writes")) work["writes"].push_back(own.resource(storage.get<std::size_t>()));

            body = {
                {      "kind",                  "operation" },
                { "operation", definition.at("body").size() }
            };
            definition["body"].push_back(std::move(work));
        }
        else {
            auto & child = regions_[item.identity];
            body = {
                {      "kind",                     "call" },
                {    "layout",               child.layout },
                {    "inputs",  local.ports(child.inputs) },
                {   "outputs", local.ports(child.outputs) },
                { "resources",    nlohmann::json::array() }
            };
            for (const auto resource : child.resources) body["resources"].push_back(local.resource(resource));
            const auto & child_metadata = source_.at("regions")[item.identity].at("metadata");
            if (child_metadata.contains("call_family")) body["family"] = child_metadata.at("call_family");

            if (!child.input_groups.empty()) {
                body["input_groups"] = nlohmann::json::object();
                for (const auto & [name, members] : child.input_groups) {
                    if (const auto group = repetitions.find(item.identity); group != repetitions.end() && group->second.contains(name)) {
                        const auto & repetition = group->second.at(name);
                        body["input_groups"][name] = {
                            { "count", repetition.count }
                        };
                        if (repetition.kind == "reuse") body["input_groups"][name]["value"] = local.value(repetition.members.front());
                        else {
                            auto & producer = regions_[repetition.producers.front()];
                            body["input_groups"][name]["producer"] = producer.layout;
                            child.frame["producers"][name] = std::move(producer.frame);
                        }
                    }
                    else if (child_metadata.contains("parent_groups") && child_metadata.at("parent_groups").contains(name)) {
                        const auto parent_group = child_metadata.at("parent_groups").at(name).get<std::string>();
                        if (region.input_groups.at(parent_group) != members) throw std::invalid_argument("child group differs from its declared parent group");
                        body["input_groups"][name] = parent_group;
                    }
                    else body["input_groups"][name] = local.ports(members);
                }
            }

            region.call_indices.emplace(item.identity, region.frame["children"].size());
            region.frame["children"].push_back(std::move(child.frame));
        }

        layout["body"].push_back(std::move(body));
    }

    // Definitions own work; layouts bind child calls and resource aliases without cloning parent work.
    layout["own_values"] = own.bind_values(local);
    layout["own_resources"] = nlohmann::json::array();
    for (const auto resource : own.resources) layout["own_resources"].push_back(local.resource(resource));

    layout["storages"] = nlohmann::json::array();
    for (const auto & value : local.values) layout["storages"].push_back(value.at("storage"));

    own.require_geometry();
    definition["values"] = std::move(own.values);
    definition["resources"] = own.resources.size();
    layout["resources"] = local.resources.size();
    region.frame["literals"] = std::move(own.literals);

    // These are session bindings, never part of a shared definition or layout identity.
    region.frame["external_resources"] = nlohmann::json::object();
    for (std::size_t resource = 0; resource < local.resources.size(); ++resource) {
        if (external_resources_.contains(local.resources[resource])) region.frame["external_resources"][std::to_string(resource)] = local.resources[resource];
    }
    region.resources = std::move(local.resources);

    const auto definition_id = library_.definitions_.try_emplace(std::move(definition), library_.definitions_.size()).first->second;
    layout["definition"] = definition_id;
    if (region.frame.contains("execution")) {
        for (auto & binding : region.frame["execution"]) {
            if (!binding.contains("guards")) continue;
            binding["candidate"] = execution_candidates_.size();
            execution_candidates_.push_back({
                { "definition",               definition_id },
                {   "literals", region.frame.at("literals") },
                {  "operation",     binding.at("operation") },
                {     "guards",        binding.at("guards") },
                {    "pattern",       binding.at("pattern") },
                {  "resources",     binding.at("resources") }
            });
            if (binding.contains("reduction")) execution_candidates_.back()["reduction"] = binding.at("reduction");
        }
    }
    region.layout = library_.layouts_.try_emplace(std::move(layout), library_.layouts_.size()).first->second;
}

std::pair<nlohmann::json, std::vector<std::size_t>> SourceTemplates::execution_selections() const {
    auto groups = nlohmann::json::array();
    std::vector<std::size_t> selected;
    std::map<nlohmann::json, std::size_t> identities;
    std::vector<std::set<nlohmann::json>> alternatives;
    std::vector<const nlohmann::json *> patterns(library_.execution_patterns_.size());
    for (const auto & [pattern, identity] : library_.execution_patterns_) patterns[identity] = &pattern;

    std::vector<std::tuple<nlohmann::json, nlohmann::json, nlohmann::json>> signatures;
    std::map<nlohmann::json, std::set<nlohmann::json>> contexts;
    for (const auto & candidate : execution_candidates_) {
        const auto & pattern = *patterns[candidate.at("pattern").get<std::size_t>()];
        const auto & resources = candidate.at("resources");
        std::set<std::size_t> context;
        auto binding_signature = nlohmann::json::array();
        for (const auto & value : resources) {
            const auto resource = value.get<std::size_t>();

            // Only events confined to one operation may change identities between equivalent prototypes.
            const bool private_event = execution_.resources().at(resource) == "event" && !external_events_.contains(resource);
            binding_signature.push_back(private_event ? nlohmann::json(nullptr) : value);
            if (!private_event) context.insert(resource);
        }
        auto guards = candidate.at("guards");
        auto conditions = nlohmann::json::array();

        // Compare semantic keys after naming symbols; executable guards keep their original evaluation order.
        for (auto & guard : guards) {
            canonicalize_expression(guard.at("condition"));
            conditions.push_back(guard.at("condition"));
        }
        const auto & call = pattern.at("host_call");
        const auto caller = call.is_null() ? nlohmann::json(nullptr)
                                           : resources.at(pattern.at("nodes").at(call.at("entry").get<std::size_t>()).at("resource").get<std::size_t>());
        nlohmann::json key{
            { "definition", candidate.at("definition") },
            {   "literals",   candidate.at("literals") },
            {  "operation",  candidate.at("operation") },
            { "conditions",                 conditions },
            {     "caller",                     caller }
        };
        if (context.size() > 1) contexts[key].insert(context);
        key["context"] = context;
        signatures.emplace_back(std::move(key), std::move(binding_signature), std::move(guards));
    }

    for (std::size_t index = 0; index < execution_candidates_.size(); ++index) {
        const auto & candidate = execution_candidates_[index];
        const auto & resources = candidate.at("resources");
        auto [key, binding_signature, guards] = std::move(signatures[index]);

        // An empty implementation has no device context; share it only with one observed calling context.
        if (key.at("context").size() == 1 && key.at("context").at(0) == key.at("caller")) {
            auto contract = key;
            contract.erase("context");
            const auto found = contexts.find(contract);
            if (found != contexts.end() && found->second.size() == 1) key["context"] = *found->second.begin();
        }
        const auto [position, added] = identities.try_emplace(key, identities.size());
        const auto identity = position->second;
        if (added) {

            // Fixed operands distinguish calling contexts without copying them into the published selection.
            key.erase("literals");
            key["choices"] = nlohmann::json::array();
            groups.push_back(std::move(key));
            alternatives.emplace_back();
        }
        selected.push_back(identity);
        nlohmann::json signature{
            {    "guards",       std::move(guards) },
            {   "pattern", candidate.at("pattern") },
            { "resources",       binding_signature }
        };
        if (candidate.contains("reduction")) signature["reduction"] = candidate.at("reduction");
        if (alternatives[identity].insert(signature).second) {
            groups[identity]["choices"].push_back({
                {    "guards",  candidate.at("guards") },
                {   "pattern", candidate.at("pattern") },
                { "resources",               resources }
            });
            if (candidate.contains("reduction")) groups[identity]["choices"].back()["reduction"] = candidate.at("reduction");
        }
    }
    return { std::move(groups), std::move(selected) };
}

void SourceTemplates::publish_selections(nlohmann::json & frame, std::span<const std::size_t> groups) {
    if (frame.contains("execution")) {
        for (auto & binding : frame["execution"]) {

            // Native observations are compilation inputs, retained once in the source capture.
            binding.erase("observation");
            if (!binding.contains("candidate")) continue;
            binding["variants"] = groups[binding.at("candidate").get<std::size_t>()];
            for (const auto * key : { "candidate", "guards", "pattern", "resources", "reduction" }) binding.erase(key);
        }
    }
    for (auto & child : frame["children"]) publish_selections(child, groups);
    if (frame.contains("producers")) {
        for (auto & [_, producer] : frame["producers"].items()) publish_selections(producer, groups);
    }
}

nlohmann::json SourceTemplates::document() {
    auto [variants, groups] = execution_selections();
    nlohmann::json result{
        {         "environment",       source_.at("environment") },
        {   "physical_coverage", source_.at("physical_coverage") },
        {    "runtime_coverage",  source_.at("runtime_coverage") },
        { "execution_resources",          execution_.resources() },
        {  "execution_variants",             std::move(variants) },
        {             "entries",         nlohmann::json::array() },
        {         "entrypoints",         nlohmann::json::array() },
        {              "inputs",         nlohmann::json::array() }
    };

    std::set<std::size_t> inputs;
    for (const auto root : roots_) {
        auto & region = regions_[root];
        auto entry = std::move(region.frame);
        publish_selections(entry, groups);
        entry.update({
            { "source_region",             root },
            {        "layout",    region.layout },
            {        "inputs",    region.inputs },
            {       "outputs",   region.outputs },
            {     "resources", region.resources }
        });
        const auto & metadata = source_.at("regions")[root].at("metadata");
        if (metadata.contains("batch")) {
            const auto & batch = source_.at("batches").at(metadata.at("batch").get<std::size_t>());
            entry["batch"] = batch;
        }

        result["entries"].push_back(std::move(entry));
        inputs.insert(region.inputs.begin(), region.inputs.end());
    }
    for (const auto input : inputs) result["inputs"].push_back(*values_.at(input));

    // Entrypoints retain a call path into the shared layout instead of duplicating nested templates.
    for (std::size_t identity = 0; identity < regions_.size(); ++identity) {
        const auto & record = source_.at("regions")[identity];
        const auto & owner = record.at("interface_owner");
        if (!owner.is_null() && owner.get<std::size_t>() != identity) continue;

        const auto & metadata = record.at("metadata");
        std::string role;
        if (metadata.contains("entrypoint")) role = metadata.at("entrypoint").get<std::string>();
        else if (record.at("role") == "module" && metadata.at("path") == "") role = "model";
        else if (record.at("parent").is_null()) role = record.at("role").get<std::string>();
        else continue;

        std::vector<std::size_t> path;
        auto root = identity;
        while (!source_.at("regions")[root].at("parent").is_null()) {
            const auto parent = source_.at("regions")[root].at("parent").get<std::size_t>();
            path.push_back(regions_[parent].call_indices.at(root));
            root = parent;
        }

        std::ranges::reverse(path);
        const auto entry = static_cast<std::size_t>(std::distance(roots_.begin(), std::ranges::find(roots_, root)));
        nlohmann::json phase;
        const auto & caller = source_.at("regions")[root].at("metadata");

        // Reservation and transfer are independent work, so their roots need no model batch or phase.
        if (metadata.contains("entrypoint_phase")) phase = metadata.at("entrypoint_phase");
        else if (caller.contains("batch")) phase = source_.at("batches").at(caller.at("batch").get<std::size_t>()).at("phase");
        result["entrypoints"].push_back({
            {  "role",  role },
            { "entry", entry },
            {  "path",  path },
            { "phase", phase }
        });
    }

    return result;
}

void TemplateLibrary::add(const nlohmann::json & source) {
    SourceTemplates compiled(source, *this);
    roles_.push_back(compiled.document());
}

void TemplateLibrary::add_instances(const nlohmann::json & instances) {
    if (!instances.is_array()) throw std::invalid_argument("role instances must be an array");
    for (const auto & instance : instances) roles_.push_back(instance);
}

nlohmann::json TemplateLibrary::document() && {
    nlohmann::json catalog{
        {        "definitions",        std::vector<nlohmann::json>(definitions_.size()) },
        {            "layouts",            std::vector<nlohmann::json>(layouts_.size()) },
        { "execution_patterns", std::vector<nlohmann::json>(execution_patterns_.size()) }
    };
    for (const auto & [definition, identity] : definitions_) catalog["definitions"][identity] = definition;
    for (const auto & [layout, identity] : layouts_) catalog["layouts"][identity] = layout;
    for (const auto & [pattern, identity] : execution_patterns_) catalog["execution_patterns"][identity] = pattern;

    return {
        {   "scope", "hierarchical_source_templates" },
        { "catalog",              std::move(catalog) },
        {   "roles",               std::move(roles_) }
    };
}

} // namespace markov
