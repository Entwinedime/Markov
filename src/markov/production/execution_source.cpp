#include "markov/production/execution_source.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <tuple>
#include <utility>

#include "markov/contracts/expression.hpp"
#include "markov/templates/group_execution.hpp"

namespace markov {
namespace {

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

} // namespace

ExecutionSource::ExecutionSource(std::map<nlohmann::json, std::size_t> & patterns, nlohmann::json domain) : patterns_(patterns), domain_(std::move(domain)) {}

bool ExecutionSource::equivalent(const nlohmann::json & left, const nlohmann::json & right) { return execution_signature(left) == execution_signature(right); }

nlohmann::json ExecutionSource::guard_signature(nlohmann::json guards) {
    for (auto & guard : guards) {

        // Diagnostic provenance does not change the condition evaluated by the consumer.
        guard = {
            { "condition", guard.at("condition") },
            {  "expected",  guard.at("expected") }
        };
        canonicalize_expression(guard.at("condition"));
    }
    std::sort(guards.begin(), guards.end());
    return guards;
}

nlohmann::json ExecutionSource::observe(const nlohmann::json & operation, std::size_t identity) {
    auto prototype = compile(operation.at("execution_observation"));
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
    const auto pattern = patterns_.try_emplace(std::move(prototype.pattern), patterns_.size()).first->second;
    nlohmann::json binding{
        {   "operation",                              identity },
        {     "pattern",                               pattern },
        {   "resources",        std::move(prototype.resources) },
        { "observation", operation.at("execution_observation") }
    };
    if (operation.contains("execution_guards")) binding["guards"] = operation.at("execution_guards");
    if (reduction) binding["reduction"] = reduction->document();
    return binding;
}

void ExecutionSource::bind(nlohmann::json & frame, std::size_t definition) {
    if (frame.contains("execution")) {
        for (auto & binding : frame["execution"]) {
            if (!domain_.is_null()) {
                nlohmann::json anchor{
                    { "definition", definition },
                    { "literals", frame.at("literals") },
                    { "operation", binding.at("operation") },
                    { "pattern", binding.at("pattern") },
                    { "guards", guard_signature(binding.value("guards", nlohmann::json::array())) }
                };
                if (binding.contains("reduction")) anchor["reduction"] = binding.at("reduction");
                anchors_[std::move(anchor)].insert(binding.at("resources"));
            }
            if (!binding.contains("guards")) continue;
            binding["candidate"] = candidates_.size();
            candidates_.push_back({
                { "definition",              definition },
                {   "literals",    frame.at("literals") },
                {  "operation", binding.at("operation") },
                {     "guards",    binding.at("guards") },
                {    "pattern",   binding.at("pattern") },
                {  "resources", binding.at("resources") }
            });
            if (binding.contains("reduction")) candidates_.back()["reduction"] = binding.at("reduction");
        }
    }
}

std::pair<nlohmann::json, std::vector<std::size_t>> ExecutionSource::selections() const {
    auto groups = nlohmann::json::array();
    std::vector<std::size_t> selected;
    std::map<nlohmann::json, std::size_t> identities;
    std::vector<std::set<nlohmann::json>> alternatives;
    std::vector<const nlohmann::json *> patterns(patterns_.size());
    for (const auto & [pattern, identity] : patterns_) patterns[identity] = &pattern;

    std::vector<std::tuple<nlohmann::json, nlohmann::json, nlohmann::json>> signatures;
    std::map<nlohmann::json, std::set<nlohmann::json>> contexts;
    for (const auto & candidate : candidates_) {
        const auto & pattern = *patterns[candidate.at("pattern").get<std::size_t>()];
        const auto & resources = candidate.at("resources");
        std::set<std::size_t> context;
        auto binding_signature = context_bindings(resources);
        for (const auto & value : binding_signature) {
            if (!value.is_null()) context.insert(value.get<std::size_t>());
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

    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        const auto & candidate = candidates_[index];
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

            // Fixed operands remain available until cross-source execution bindings have been resolved.
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

void ExecutionSource::publish(nlohmann::json & frame, std::span<const std::size_t> groups) {
    if (frame.contains("execution")) {
        for (auto & binding : frame["execution"]) {

            // Native observations are compilation inputs, retained once in the source capture.
            binding.erase("observation");
            if (!binding.contains("candidate")) continue;
            binding["variants"] = groups[binding.at("candidate").get<std::size_t>()];
            for (const auto * key : { "candidate", "guards", "pattern", "resources", "reduction" }) binding.erase(key);
        }
    }
    for (auto & child : frame["children"]) publish(child, groups);
    if (frame.contains("producers")) {
        for (auto & [_, producer] : frame["producers"].items()) publish(producer, groups);
    }
}

nlohmann::json ExecutionSource::finalize(nlohmann::json & entries) const {
    auto [variants, groups] = selections();
    for (auto & entry : entries) publish(entry, groups);
    return variants;
}

} // namespace markov
