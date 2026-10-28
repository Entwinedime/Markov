#include "markov/execution/protocol.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace markov {
namespace {

constexpr std::array<std::string_view, 17> kind_names{ "enqueue",     "submit",       "device",       "copy",         "copy_async",
                                                       "wait_stream", "wait_device",  "transfer",     "queue_return", "unclassified",
                                                       "host",        "wait_entry",   "record_event", "wait_event",   "wait_event_complete",
                                                       "barrier",     "communication" };

} // namespace

ExecutionKind execution_kind(std::string_view name) {
    const auto kind = std::ranges::find(kind_names, name);
    if (kind == kind_names.end()) throw std::invalid_argument("unknown execution action: " + std::string(name));
    return static_cast<ExecutionKind>(kind - kind_names.begin());
}

std::string_view execution_name(ExecutionKind kind) { return kind_names.at(static_cast<std::size_t>(kind)); }

ExecutionPattern ExecutionPattern::read(const nlohmann::json & source) {
    ExecutionPattern result;
    result.sequences = source.at("sequences").get<std::vector<std::vector<std::size_t>>>();

    for (const auto & node : source.at("nodes")) {
        const auto name = node.at("kind").get<std::string>();
        ExecutionStep step{ execution_kind(name), node.at("resource").get<std::size_t>(), std::nullopt, node.at("name").get<std::string>() };
        if (node.contains("target")) step.target = node.at("target").get<std::size_t>();
        if (node.contains("work_part")) step.part = node.at("work_part").get<std::size_t>();
        if (step.resource >= result.sequences.size() || (step.target && *step.target >= result.sequences.size()))
            throw std::invalid_argument("execution action references an undefined resource");
        result.steps.push_back(std::move(step));
    }

    // Every action belongs to exactly one ordered resource sequence.
    std::vector<bool> seen(result.steps.size());
    for (std::size_t resource = 0; resource < result.sequences.size(); ++resource) {
        for (const auto node : result.sequences[resource]) {
            if (result.steps.at(node).resource != resource || seen[node]) throw std::invalid_argument("execution resource sequence has a conflicting action");
            seen[node] = true;
        }
    }
    if (std::ranges::find(seen, false) != seen.end()) throw std::invalid_argument("execution action has no resource sequence");

    for (const auto & link : source.at("links")) {
        const auto from = link.at("source").get<std::size_t>();
        const auto to = link.at("target").get<std::size_t>();
        if (from >= result.steps.size() || to >= result.steps.size() || from == to) throw std::invalid_argument("invalid execution link");
        result.links.push_back({ from, to, link.at("kind").get<std::string>() });
    }

    for (const auto & required : source.at("requirements")) {
        ExecutionRequirement requirement{ required.at("resource").get<std::size_t>(),
                                          std::nullopt,
                                          required.at("target").get<std::size_t>(),
                                          required.at("kind").get<std::string>() };
        if (!required.at("source").is_null()) requirement.source = required.at("source").get<std::size_t>();
        if (requirement.resource >= result.sequences.size() || requirement.target >= result.steps.size()
            || (requirement.source && (result.steps.at(*requirement.source).resource != requirement.resource || *requirement.source == requirement.target)))
            throw std::invalid_argument("invalid execution frontier requirement");

        const auto kind = result.steps[requirement.target].kind;
        const bool valid = (requirement.kind == "queue_drain" && kind == ExecutionKind::WaitEntry)
                           || (requirement.kind == "wait_completion" && (kind == ExecutionKind::WaitStream || kind == ExecutionKind::WaitDevice))
                           || (requirement.kind == "event_wait" && !requirement.source && result.sequences[requirement.resource].empty()
                               && (kind == ExecutionKind::Barrier || kind == ExecutionKind::WaitEventComplete));
        if (!valid) throw std::invalid_argument("execution frontier kind conflicts with its target or resource");
        result.requirements.push_back(std::move(requirement));
    }

    std::set<std::size_t> signals;
    for (const auto & signal : source.at("signals")) {
        const ExecutionSignal port{ signal.at("resource").get<std::size_t>(), signal.at("source").get<std::size_t>() };
        const auto kind = result.steps.at(port.source).kind;
        if (port.resource >= result.sequences.size() || !result.sequences[port.resource].empty() || !signals.insert(port.resource).second
            || kind != ExecutionKind::Barrier)
            throw std::invalid_argument("invalid execution event signal");
        result.signals.push_back(port);
    }

    const auto & communication = source.at("communication");
    if (!communication.is_null()) {
        const ExecutionPorts ports{ communication.at("entry").get<std::size_t>(), communication.at("return").get<std::size_t>() };
        const auto & entry = result.steps.at(ports.entry);
        const auto & returned = result.steps.at(ports.returned);
        if (entry.kind != ExecutionKind::Barrier || returned.kind != ExecutionKind::Barrier || entry.resource != returned.resource
            || ports.entry == ports.returned)
            throw std::invalid_argument("communication ports must delimit work on one stream");
        result.communication = ports;
    }

    const auto & call = source.at("host_call");
    if (!call.is_null()) {
        const ExecutionPorts ports{ call.at("entry").get<std::size_t>(), call.at("return").get<std::size_t>() };
        const auto & entry = result.steps.at(ports.entry);
        const auto & returned = result.steps.at(ports.returned);
        const bool opaque_service = ports.entry == ports.returned && entry.kind == ExecutionKind::Transfer;
        if ((!opaque_service && (entry.kind != ExecutionKind::Host || returned.kind != ExecutionKind::Host)) || entry.resource != returned.resource
            || result.sequences[entry.resource].front() != ports.entry || result.sequences[entry.resource].back() != ports.returned)
            throw std::invalid_argument("host call ports do not delimit its caller service sequence");
        result.host_call = ports;
    }

    return result;
}

void ExecutionOwner::rebase(std::size_t operation_offset, std::size_t region_offset) { identity += kind == Kind::Operation ? operation_offset : region_offset; }

const char * ExecutionOwner::key() const { return kind == Kind::Operation ? "operation" : "region"; }

void ExecutionGraph::call(ExecutionOwner owner, ExecutionPorts ports) { host_calls_.try_emplace(owner, ports).first->second.returned = ports.returned; }

void ExecutionGraph::sequence(std::size_t node, std::size_t resource) {
    heads_.try_emplace(resource, node);
    if (const auto previous = tails_.find(resource); previous != tails_.end()) dependencies_.push_back({ previous->second, node, "resource_order" });
    tails_.insert_or_assign(resource, node);
}

void ExecutionGraph::depend(std::size_t source, std::size_t target, std::string kind) { dependencies_.push_back({ source, target, std::move(kind) }); }

std::vector<std::size_t> ExecutionGraph::initial_nodes(std::size_t begin) const {
    std::set<std::size_t> dependent;
    for (const auto & edge : dependencies_) {
        if (edge.source >= begin && edge.target >= begin) dependent.insert(edge.target);
    }

    std::vector<std::size_t> result;
    for (auto node = begin; node < first_node_ + nodes_.size(); ++node) {
        if (!dependent.contains(node)) result.push_back(node);
    }
    return result;
}

std::vector<std::size_t> ExecutionGraph::terminal_nodes(std::size_t begin) const {
    std::set<std::size_t> continued;
    for (const auto & edge : dependencies_) {
        if (edge.source >= begin && edge.target >= begin) continued.insert(edge.source);
    }

    std::vector<std::size_t> result;
    for (auto node = begin; node < first_node_ + nodes_.size(); ++node) {
        if (!continued.contains(node)) result.push_back(node);
    }
    return result;
}

void ExecutionGraph::append_host(std::size_t region, std::size_t resource) {
    const auto node = first_node_ + nodes_.size();
    const ExecutionOwner owner{ ExecutionOwner::Kind::Region, region };
    nodes_.push_back({
        { ExecutionKind::Host, resource, std::nullopt, "self" },
        owner
    });
    sequence(node, resource);
    call(owner, { node, node });
}

void ExecutionGraph::initially_idle(std::size_t resource) { initially_idle_.insert(resource); }

void ExecutionGraph::append(const ExecutionPattern & pattern, std::span<const std::size_t> resources, std::size_t operation, std::size_t region) {
    const auto offset = first_node_ + nodes_.size();
    for (auto step : pattern.steps) {
        step.resource = resources[step.resource];
        if (step.target) step.target = resources[*step.target];
        nodes_.push_back({
            std::move(step),
            { ExecutionOwner::Kind::Operation, operation }
        });
    }

    if (pattern.host_call) {
        const ExecutionPorts ports{ offset + pattern.host_call->entry, offset + pattern.host_call->returned };
        call({ ExecutionOwner::Kind::Operation, operation }, ports);
        call({ ExecutionOwner::Kind::Region, region }, ports);
    }
    for (const auto & link : pattern.links) dependencies_.push_back({ offset + link.source, offset + link.target, link.kind });

    // Capture the incoming frontier before installing this operation's later work on the same resource.
    for (const auto & required : pattern.requirements) {
        const auto resource = resources[required.resource];
        auto source = required.source ? std::optional(offset + *required.source) : std::nullopt;
        if (!source && tails_.contains(resource)) source = tails_.at(resource);
        if (source) dependencies_.push_back({ *source, offset + required.target, required.kind });
        else incoming_.push_back({ resource, std::nullopt, offset + required.target, required.kind });
    }

    for (std::size_t local = 0; local < pattern.sequences.size(); ++local) {
        const auto resource = resources[local];
        for (const auto node : pattern.sequences[local]) {
            sequence(offset + node, resource);

            // Data becomes ready at compute/transfer completion, independently of host call return.
            const auto kind = pattern.steps[node].kind;
            if (kind == ExecutionKind::Device || kind == ExecutionKind::Transfer || kind == ExecutionKind::Copy) {
                auto & ports = data_ports_[operation].try_emplace(resource, ExecutionPorts{ offset + node, offset + node }).first->second;
                ports.returned = offset + node;
            }
        }
    }

    // An event publishes a completion token; recording it does not serialize the recording stream with an earlier record.
    for (const auto & signal : pattern.signals) tails_.insert_or_assign(resources[signal.resource], offset + signal.source);

    if (pattern.communication) {
        const ExecutionPorts ports{ offset + pattern.communication->entry, offset + pattern.communication->returned };
        communications_.emplace(operation, ports);
        data_ports_[operation].emplace(resources[pattern.steps[pattern.communication->entry].resource], ports);
    }
}

void ExecutionGraph::append(ExecutionGraph fragment, std::size_t operation_offset, std::size_t region_offset, std::size_t resource_offset) {
    const auto offset = first_node_ + nodes_.size();
    for (auto required : fragment.incoming_) {
        required.target += offset;
        required.resource += resource_offset;
        if (const auto source = tails_.find(required.resource); source != tails_.end())
            dependencies_.push_back({ source->second, required.target, required.kind });
        else if (!initially_idle_.contains(required.resource) || (required.kind != "wait_completion" && required.kind != "queue_drain"))
            incoming_.push_back(std::move(required));
    }

    // Fragments from one asset share logical execution resources, unlike their private tensor storage.
    for (const auto & [resource, head] : fragment.heads_) {
        heads_.try_emplace(resource + resource_offset, offset + head);
        if (const auto previous = tails_.find(resource + resource_offset); previous != tails_.end())
            dependencies_.push_back({ previous->second, offset + head, "resource_order" });
    }
    for (const auto & [resource, tail] : fragment.tails_) tails_.insert_or_assign(resource + resource_offset, offset + tail);

    // Rebase private node and owner identities before moving the fragment into this graph.
    for (auto & node : fragment.nodes_) {
        node.owner.rebase(operation_offset, region_offset);
        node.step.resource += resource_offset;
        if (node.step.target) *node.step.target += resource_offset;
    }
    for (const auto & [source, ports] : fragment.host_calls_) {
        auto owner = source;
        owner.rebase(operation_offset, region_offset);
        host_calls_.emplace(owner, ExecutionPorts{ offset + ports.entry, offset + ports.returned });
    }
    for (auto & [operation, calls] : fragment.data_ports_) {
        std::map<std::size_t, ExecutionPorts> rebased;
        for (auto & [resource, ports] : calls) {
            ports.entry += offset;
            ports.returned += offset;
            rebased.emplace(resource + resource_offset, ports);
        }
        data_ports_.emplace(operation + operation_offset, std::move(rebased));
    }
    for (const auto & [operation, ports] : fragment.communications_)
        communications_.emplace(operation + operation_offset, ExecutionPorts{ offset + ports.entry, offset + ports.returned });

    for (auto & edge : fragment.dependencies_) {
        edge.source += offset;
        edge.target += offset;
    }

    nodes_.insert(nodes_.end(), std::make_move_iterator(fragment.nodes_.begin()), std::make_move_iterator(fragment.nodes_.end()));
    dependencies_.insert(dependencies_.end(), std::make_move_iterator(fragment.dependencies_.begin()), std::make_move_iterator(fragment.dependencies_.end()));
}

void ExecutionGraph::connect_communication(std::span<const std::size_t> operations, std::size_t resource, std::size_t region, const std::string & kind) {
    const auto node = first_node_ + nodes_.size();
    nodes_.push_back({
        { ExecutionKind::Communication, resource, std::nullopt, kind },
        { ExecutionOwner::Kind::Region, region }
    });

    // A shared transfer waits for its actual endpoints and releases their local transport streams.
    for (const auto operation : operations) {
        const auto ports = communications_.at(operation);
        dependencies_.push_back({ ports.entry, node, kind + "_ready" });
        dependencies_.push_back({ node, ports.returned, kind + "_complete" });
        communications_.erase(operation);
    }
}

void ExecutionGraph::connect_accesses(std::span<const ValueAccess> values, std::span<const nlohmann::json> types, std::span<const Operation> operations) {
    for (; bound_operations_ < operations.size(); ++bound_operations_) {
        const auto identity = bound_operations_;
        const auto & operation = operations[identity];
        const auto caller = host_calls_.find({ ExecutionOwner::Kind::Operation, identity });
        if (caller == host_calls_.end()) {
            missing_call_ports_ = true;
            continue;
        }

        std::set<std::size_t> producers;
        std::set<std::size_t> reads;
        std::set<std::size_t> writes(operation.writes.begin(), operation.writes.end());
        std::set<std::tuple<std::size_t, std::size_t, std::string_view>> links;
        std::map<std::size_t, bool> host_memory;
        for (const auto input : operation.inputs) {
            const auto & value = values[input];
            if (value.producer) producers.insert(*value.producer);
            else if (const auto ready = value_ready_.find(input); ready != value_ready_.end()) links.emplace(ready->second, caller->second.entry, "value");
            reads.insert(value.storage);
            host_memory.emplace(value.storage, types[input].at("device") == "cpu");
        }
        for (const auto output : operation.outputs) {
            const auto & value = values[output];
            host_memory.emplace(value.storage, types[output].at("device") == "cpu");

            // New storage has a data producer; a view only creates another tensor descriptor.
            if (value.producer == identity && !reads.contains(value.storage)) writes.insert(value.storage);
        }
        for (const auto storage : writes) {
            if (!host_memory.contains(storage)) throw std::invalid_argument("operation write has no typed tensor operand");
        }

        for (const auto producer : producers) {
            if (producer >= identity) throw std::invalid_argument("operation input is not an earlier value");
            const auto source = host_calls_.find({ ExecutionOwner::Kind::Operation, producer });
            if (source == host_calls_.end()) missing_call_ports_ = true;
            else links.emplace(source->second.returned, caller->second.entry, "value");
        }

        auto data = data_ports_.extract(identity);
        const auto device_ports = data.empty() ? std::map<std::size_t, ExecutionPorts>{} : std::move(data.mapped());
        auto host_ports = device_ports;

        // shortcut: CPU operands span caller and transfers; refine when an interface exposes narrower access ports.
        host_ports.insert_or_assign(nodes_[caller->second.entry - first_node_].step.resource, caller->second);
        for (const auto & [storage, host] : host_memory) {
            if (!reads.contains(storage) && !writes.contains(storage)) continue;
            const auto & ports = host ? host_ports : device_ports;
            if (ports.empty()) continue;

            auto & frontier = storage_frontiers_[storage];
            const bool writing = writes.contains(storage);
            for (const auto & [_, target] : ports) {
                for (const auto & [source_resource, source] : frontier.writers)
                    links.emplace(source, target.entry, writing ? "write_after_write" : "read_after_write");
                if (writing) {
                    for (const auto & [source_resource, source] : frontier.readers) links.emplace(source, target.entry, "write_after_read");
                }
            }

            if (writing) {
                frontier.writers.clear();
                frontier.readers.clear();
            }
            auto & completed = writing ? frontier.writers : frontier.readers;
            for (const auto & [resource, access] : ports) completed.insert_or_assign(resource, access.returned);
        }

        // Several tensor operands can require the same execution edge.
        for (const auto & [source, target, kind] : links) dependencies_.push_back({ source, target, std::string(kind) });
    }
}

std::vector<std::size_t> ExecutionGraph::retire(std::span<const ValueAccess> values, const std::map<std::size_t, std::size_t> & retained_values,
                                                const std::set<std::size_t> & retained_storage, const std::map<std::size_t, std::size_t> & retained_regions,
                                                const std::map<std::size_t, std::size_t> & retained_operations) {
    std::map<std::size_t, std::size_t> ready;
    for (const auto & [old_value, new_value] : retained_values) {
        const auto & value = values[old_value];
        if (value.producer) ready.emplace(new_value, host_calls_.at({ ExecutionOwner::Kind::Operation, *value.producer }).returned);
        else if (const auto previous = value_ready_.find(old_value); previous != value_ready_.end()) ready.emplace(new_value, previous->second);
    }
    value_ready_ = std::move(ready);
    std::erase_if(storage_frontiers_, [&](const auto & item) { return !retained_storage.contains(item.first); });

    // Frontiers may still be running; the executor retains them after this graph releases its history.
    std::set<std::size_t> frontier;
    for (const auto & [_, node] : tails_) frontier.insert(node);
    for (const auto & [_, node] : value_ready_) frontier.insert(node);
    for (const auto & [_, storage] : storage_frontiers_) {
        for (const auto & [resource, node] : storage.writers) frontier.insert(node);
        for (const auto & [resource, node] : storage.readers) frontier.insert(node);
    }

    // A suspended host call resumes with the same entry and resource sequence after its children are retired.
    std::map<ExecutionOwner, ExecutionPorts> calls;
    for (const auto & [old_region, new_region] : retained_regions) {
        const auto ports = region_call(old_region);
        calls.emplace(ExecutionOwner{ ExecutionOwner::Kind::Region, new_region }, ports);
        frontier.insert(ports.entry);
        frontier.insert(ports.returned);
    }

    // Unpaired endpoints keep their geometry and execution ports until the peer arrives.
    std::map<std::size_t, ExecutionPorts> communications;
    for (const auto & [old_operation, new_operation] : retained_operations) {
        const auto ports = host_calls_.at({ ExecutionOwner::Kind::Operation, old_operation });
        calls.emplace(ExecutionOwner{ ExecutionOwner::Kind::Operation, new_operation }, ports);
        frontier.insert(ports.entry);
        frontier.insert(ports.returned);
    }
    for (const auto & [operation, ports] : communications_) {
        communications.emplace(retained_operations.at(operation), ports);
        frontier.insert(ports.entry);
        frontier.insert(ports.returned);
    }

    first_node_ += nodes_.size();
    nodes_.clear();
    dependencies_.clear();
    heads_.clear();
    host_calls_ = std::move(calls);
    communications_ = std::move(communications);
    bound_operations_ = retained_operations.size();
    return { frontier.begin(), frontier.end() };
}

std::size_t ExecutionGraph::first_node() const { return first_node_; }
std::span<const ExecutionNode> ExecutionGraph::nodes() const { return nodes_; }
std::span<const Dependency> ExecutionGraph::dependencies() const { return dependencies_; }
ExecutionPorts ExecutionGraph::region_call(std::size_t region) const { return host_calls_.at({ ExecutionOwner::Kind::Region, region }); }
std::span<const ExecutionRequirement> ExecutionGraph::incoming_waits() const { return incoming_; }
bool ExecutionGraph::has_unbound_accesses(std::size_t operation_count) const { return missing_call_ports_ || bound_operations_ != operation_count; }
const std::map<std::size_t, ExecutionPorts> & ExecutionGraph::pending_communications() const { return communications_; }

nlohmann::json ExecutionGraph::document() const {
    nlohmann::json result{
        {        "scope", "instantiated_source_execution_relations" },
        {       "status",                                 "partial" },
        {        "nodes",                   nlohmann::json::array() },
        { "dependencies",                   nlohmann::json::array() },
        {   "host_calls",                   nlohmann::json::array() },
        {     "incoming",                   nlohmann::json::array() }
    };
    result["readiness"] = {
        {      "scope",           "whole_storage_operation_frontiers" },
        {     "status", missing_call_ports_ ? "partial" : "connected" },
        { "operations",                             bound_operations_ }
    };
    result["communications"] = nlohmann::json::array();
    for (const auto & [operation, ports] : communications_)
        result["communications"].push_back({
            { "operation",      operation },
            {     "entry",    ports.entry },
            {    "return", ports.returned }
        });

    for (std::size_t identity = 0; identity < nodes_.size(); ++identity) {
        const auto & node = nodes_[identity];
        const auto & step = node.step;
        nlohmann::json record{
            {             "id",                             first_node_ + identity },
            { node.owner.key(),                                node.owner.identity },
            {           "kind", kind_names.at(static_cast<std::size_t>(step.kind)) },
            {           "name",                                          step.name },
            {       "resource",                                      step.resource }
        };
        if (step.target) record["target_resource"] = *step.target;
        if (step.part) record["work_part"] = *step.part;
        result["nodes"].push_back(std::move(record));
    }

    for (const auto & link : dependencies_)
        result["dependencies"].push_back({
            { "source", link.source },
            { "target", link.target },
            {   "kind",   link.kind }
        });

    for (const auto & [owner, call] : host_calls_)
        result["host_calls"].push_back({
            { owner.key(), owner.identity },
            {     "entry",     call.entry },
            {    "return",  call.returned }
        });

    for (const auto & required : incoming_)
        result["incoming"].push_back({
            { "resource", required.resource },
            {   "target",   required.target },
            {     "kind",     required.kind }
        });

    return result;
}

} // namespace markov
