#include "markov/adapters/frameworks/sglang/structure_observation.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/adapters/runtimes/ascend/source_extractor.hpp"
#include "markov/contracts/batch_membership.hpp"
#include "markov/graph/cpu_service_partition.hpp"
#include "markov/graph/dependency_paths.hpp"
#include "markov/graph/directed_graph.hpp"
#include "markov/graph/kernel_shapes.hpp"
#include "markov/templates/communication_work.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {
namespace {
using Json = nlohmann::json;

Json native_workspace_pool(const Json & record) {
    return Json::array({ record.at("rank"), record.at("pid"), record.at("workspace").at("device"), record.at("stream_handle") });
}

bool native_host_wait(std::string_view name) {
    return name == "rtStreamSynchronize" || name == "rtStreamSynchronizeWithTimeout" || name == "rtDeviceSynchronize"
           || name == "rtDeviceSynchronizeWithTimeout" || name == "rtEventSynchronize";
}

void finding(Json & report, const Json & detail) {
    report["total"] = report.at("total").get<std::size_t>() + 1;
    if (detail.is_object() && detail.contains("reason")) {
        auto & count = report["by_reason"][detail.at("reason").get<std::string>()];
        count = count.is_null() ? 1 : count.get<std::size_t>() + 1;
    }
    if (report.at("examples").size() < 5) {
        report["examples"].push_back(detail);
        report["shown"] = report.at("examples").size();
    }
}

class ObservedState {
public:
    void compare(std::string_view field, const Json & target, const Json & observed, bool effect = false) {
        data["checked"].push_back(field);
        if (target != observed)
            data[effect ? "effects" : "differences"].push_back({
                {    "field",    field },
                {   "target",   target },
                { "observed", observed }
            });
    }

    void unknown(std::string_view field) { data["unobserved"].push_back(field); }

    Json data{
        {             "checked", Json::array() },
        {          "unobserved", Json::array() },
        {         "differences", Json::array() },
        {             "effects", Json::array() },
        { "resource_boundaries", Json::array() }
    };
};
} // namespace

SGLangStructureObservation::SGLangStructureObservation(const AscendSourceExtractor & source, const SGLangBindings & bindings, std::uint32_t request_capacity)
    : source_(source),
      bindings_(bindings),
      request_capacity_(request_capacity),
      regions_(source.calls().size()),
      collectives_(source.calls().size()),
      collective_implementations_(source.calls().size()),
      cpu_services_(source.calls().size()),
      workspace_uses_(source.calls().size()) {
    const auto calls = source.calls();
    for (std::size_t index = 0; index < calls.size(); ++index) {
        auto owner = index;
        while (calls[owner].is_annotation()) owner = calls[owner].parent.value();
        serving_calls_.push_back(owner);
        if (calls[index].kind == SourceCallKind::NativeRegion) regions_[owner].push_back(index);
    }
    for (auto & regions : regions_) std::ranges::sort(regions, {}, [&](auto index) { return calls[index].observed_start_ns; });

    std::map<std::string, std::size_t> owners;
    for (std::size_t index = 0; index < calls.size(); ++index) owners.emplace(calls[index].identity, serving_call(index));
    const auto & facts = source.facts();
    for (std::size_t index = 0; index < facts.at("native_api").size(); ++index) {
        const auto & record = facts.at("native_api")[index];
        if (!record.contains("workspace") || record.at("owner_call").is_null()) continue;
        const auto owner = owners.find(record.at("owner_call").get<std::string>());
        if (owner == owners.end()) continue;
        workspace_uses_[owner->second].push_back(index);
        const auto [last, inserted] = workspace_last_.try_emplace(native_workspace_pool(record), index);
        if (!inserted && facts.at("native_api")[last->second].at("start_ns") < record.at("start_ns")) last->second = index;
    }
    for (const auto & window : facts.at("workspace_states"))
        if (!window.at("probe_installation").is_null() && !window.at("initial").is_null() && !window.at("final").is_null())
            workspace_windows_.emplace(std::pair{ window.at("rank").get<std::uint32_t>(), window.at("pid").get<std::uint64_t>() }, &window);
    read_collectives();
    read_input_messages();
    read_cpu_services();
}

void SGLangStructureObservation::read_input_messages() {
    const auto calls = source_.calls();
    const auto & graph = source_.graph();
    const auto & records = source_.facts().at("calls");
    std::map<std::size_t, HostMessageRule> message_rules;
    std::vector<std::string_view> semantics(calls.size());
    std::map<std::size_t, std::vector<std::size_t>> children;
    for (std::size_t call = 0; call < calls.size(); ++call) {
        const auto & native = calls[call];
        if (native.kind == SourceCallKind::NativeRegion && native.binding) semantics[call] = bindings_.region(*native.binding).semantic;
        if (native.parent && (semantics[call] == "input_message_submit" || semantics[call] == "input_message_wait")) children[*native.parent].push_back(call);
    }
    const auto semantic = [&](std::size_t call) { return semantics[call]; };

    // Native scope ownership locates completion work once, independently of
    // message count and the size of the model's device graph.
    std::map<std::size_t, std::vector<SourceSpanId>> completion_spans;
    for (SourceSpanId span = 0; span < graph.spans().size(); ++span) {
        const auto & work = graph.spans()[span];
        if (work.kind != SourceSpanKind::CpuControl && work.kind != SourceSpanKind::HostWait) continue;
        for (auto ancestor = work.scope_call; ancestor && calls[*ancestor].is_annotation(); ancestor = calls[*ancestor].parent)
            if (semantic(*ancestor) == "input_message_submit" || semantic(*ancestor) == "input_message_wait") completion_spans[*ancestor].push_back(span);
    }
    for (std::size_t call = 0; call < calls.size(); ++call) {
        if (semantic(call) != "input_message") continue;
        const auto & message = records.at(calls[call].identity).at("metadata").at("message");
        const auto owner = serving_call(call);
        const auto gap = [&](std::string_view reason) {
            finding(collective_gaps_,
                    {
                        { "reason",               reason },
                        {   "call", calls[call].identity }
            });
        };
        const auto ranks = message.at("group_ranks").get<std::vector<std::uint32_t>>();
        const auto root = message.at("root").get<std::uint32_t>();
        if (!calls[call].marker || !calls[call].parent || semantic(*calls[call].parent) != "input_broadcast" || !message.at("synchronous").get<bool>()
            || message.at("rank") != calls[call].rank || std::ranges::find(ranks, root) == ranks.end()
            || std::ranges::find(ranks, calls[call].rank) == ranks.end() || std::set(ranks.begin(), ranks.end()).size() != ranks.size()) {
            gap("input_message_native_group_or_scope_not_observed");
            continue;
        }
        const auto dependencies = message.find("completion_entry_dependencies");
        if (dependencies == message.end() || dependencies->is_null()) {
            gap("host_message_completion_dependencies_not_observed");
            continue;
        }
        const auto message_rule = HostMessageRule::bind(root, ranks, *dependencies);
        if (!message_rule) {
            gap(message_rule.error());
            continue;
        }
        message_rules.emplace(call, *message_rule);
        std::optional<std::size_t> submission, wait;
        bool unique = true;
        for (const auto child : children[call]) {
            const bool submitting = semantic(child) == "input_message_submit";
            auto & slot = submitting ? submission : wait;
            if (slot || !calls[child].marker || records.at(calls[child].identity).at("metadata").at("message") != message) unique = false;
            slot = child;
        }
        if (!unique || !submission) {
            gap("input_message_native_submission_not_unique");
            continue;
        }
        const auto & result = records.at(calls[*submission].identity).at("result");
        if (!result.contains("returns_work") || result.at("returns_work").get<bool>() != wait.has_value()) {
            gap("input_message_native_completion_not_observed");
            continue;
        }
        const auto completion = wait.value_or(*submission);
        const auto & host = graph.spans()[*calls[call].marker];
        const auto & launch = graph.spans()[*calls[*submission].marker];
        const auto & completed = graph.spans()[*calls[completion].marker];
        CollectiveWork work{
            "broadcast",
            ranks,
            {},
            message.at("data_type").get<std::string>(),
            HostMessageWork{ root, std::nullopt, {} }
        };
        for (const auto & request : calls[owner].incoming_requests)
            work.host_message->incoming_requests.emplace_back(request.prompt_tokens, request.output_tokens);
        auto logical_scope = scope(call);
        logical_scope["collective_position"] = message.at("channel").get<std::string>() + "/" + std::to_string(message.at("ordinal").get<std::uint32_t>());
        collectives_[owner].push_back(
            { call, *calls[call].marker, std::move(work), std::move(logical_scope), host.begin, host.end, { launch.begin }, { completed.end } });

        collective_implementations_[owner].push_back({ call, std::move(completion_spans[completion]), true });
    }

    // Native group ordering identifies this broadcast across actual ranks.
    // Independent PG1 role sessions never prove a cross-rank completion.
    using Member = std::pair<std::size_t, std::size_t>;
    std::map<Json, std::vector<Member>> messages;
    for (std::size_t owner = 0; owner < collectives_.size(); ++owner)
        for (std::size_t index = 0; index < collectives_[owner].size(); ++index) {
            const auto & observed = collectives_[owner][index];
            if (!observed.work.host_message) continue;
            const auto & message = records.at(calls[observed.call].identity).at("metadata").at("message");
            messages[Json::array({ message.at("group_ranks"), message.at("channel"), message.at("broadcast_sequence"), message.at("ordinal") })].emplace_back(
                owner,
                index);
        }
    std::set<std::size_t> qualified;
    for (const auto & [identity, members] : messages) {
        const auto & first = collectives_[members.front().first][members.front().second];
        const auto & first_message = records.at(calls[first.call].identity).at("metadata").at("message");
        const auto root = first.work.host_message->root;
        std::map<std::uint32_t, BoundaryId> entries;
        bool group = members.size() == first.work.ranks.size();
        const auto & incoming = records.at(calls[members.front().first].identity).at("result").at("input_requests");
        for (const auto & [owner, index] : members) {
            const auto & observed = collectives_[owner][index];
            const auto & message = records.at(calls[observed.call].identity).at("metadata").at("message");
            group = group && observed.work == first.work && observed.entries.size() == 1 && message.at("physical_group_size") == first.work.ranks.size()
                    && message.at("element_count") == first_message.at("element_count") && message.at("wire_bytes") == first_message.at("wire_bytes")
                    && message_rules.at(observed.call) == message_rules.at(first.call)
                    && records.at(calls[owner].identity).at("result").at("input_requests") == incoming;
            if (observed.entries.size() == 1 && !entries.emplace(calls[observed.call].rank, observed.entries.front()).second) group = false;
        }
        group = group && entries.contains(root);
        if (group) {
            for (const auto & [owner, index] : members) {
                auto & observed = collectives_[owner][index];
                if (calls[observed.call].rank != root) observed.entries.push_back(entries.at(root));
                // The adapter derives protocol constraints from the pinned
                // implementation. Entry and return cuts themselves are native
                // observations; these links are not independent transport data.
                for (const auto & [entry_rank, done_rank] : message_rules.at(observed.call).completion_entry_dependencies)
                    if (done_rank == calls[observed.call].rank)
                        observed.completion_entry_dependencies.emplace_back(entries.at(entry_rank), observed.completions.front());
                qualified.insert(observed.call);
            }
        }
        else {
            for (const auto & [owner, index] : members)
                finding(collective_gaps_,
                        {
                            { "reason", "input_message_native_group_frontier_not_observed" },
                            {   "call",    calls[collectives_[owner][index].call].identity }
                });
        }
    }
    for (auto & operations : collectives_)
        std::erase_if(operations, [&](const auto & operation) { return operation.work.host_message && !qualified.contains(operation.call); });
    for (auto & implementations : collective_implementations_)
        for (auto & implementation : implementations)
            if (semantic(implementation.call) == "input_message") implementation.qualified = qualified.contains(implementation.call);
}

void SGLangStructureObservation::read_cpu_services() {
    const auto & graph = source_.graph();
    const auto calls = source_.calls();
    const auto & facts = source_.facts();
    Json gaps{
        {     "total",              0 },
        {     "shown",              0 },
        { "by_reason", Json::object() },
        {  "examples",  Json::array() }
    };
    CpuServicePartition partition(graph.boundaries().size());
    for (const auto & [point, name] : graph.cpu_boundaries()) partition.name(point, &name);
    for (const auto & edge : graph.edges()) partition.connect(edge.source, edge.target, edge.relation);

    for (std::size_t call = 0; call < calls.size(); ++call) {
        const auto semantic =
            calls[call].binding && calls[call].kind == SourceCallKind::NativeRegion ? bindings_.region(*calls[call].binding).semantic : std::string_view{};
        // These children retain actual enqueue/wait coordinates for ownership.
        // Their names alone do not split a measurable CPU service.
        const bool message_child = semantic == "input_message_submit" || semantic == "input_message_wait";
        if (calls[call].marker && bindings_.cpu_work_boundary(calls[call]) && !message_child) {
            const auto & marker = graph.spans()[*calls[call].marker];
            partition.protect(marker.begin);
            partition.protect(marker.end);
        }
        if (calls[call].is_annotation()) continue;
        if (!calls[call].marker)
            finding(gaps,
                    {
                        { "reason", "native_cpu_scope_marker_not_observed" },
                        {   "call",                   calls[call].identity }
            });
        for (const auto & [port, point] : bindings_.ports(graph, calls, call)) partition.protect(point);

        // Match the actual request handoff, not every output wrapper return.
        if (const auto publication = output(call); publication && !publication->requests.empty()) partition.protect(publication->end);
    }
    for (const auto & operations : collectives_)
        for (const auto & operation : operations)
            if (operation.work.host_message) {
                for (const auto point : operation.entries) partition.protect(point);
                for (const auto point : operation.completions) partition.protect(point);
            }

    std::set<SourceSpanId> host_transport;
    for (const auto & implementations : collective_implementations_)
        for (const auto & implementation : implementations)
            if (calls[implementation.call].binding && bindings_.region(*calls[implementation.call].binding).semantic == "input_message")
                host_transport.insert(implementation.spans.begin(), implementation.spans.end());
    std::vector<SourceSpanId> candidates;
    for (SourceSpanId identity = 0; identity < graph.spans().size(); ++identity) {
        if (host_transport.contains(identity)) continue;
        const auto & span = graph.spans()[identity];
        if (span.kind == SourceSpanKind::HostWait) partition.wait(span.begin, span.end);
        else if (span.kind == SourceSpanKind::CpuControl && !graph.transfers().contains(identity) && span.owner_call) {
            candidates.push_back(identity);
            partition.add({ identity, span.begin, span.end, *span.owner_call, calls[*span.owner_call].rank });
        }
        else if (span.kind != SourceSpanKind::CpuObservation && span.kind != SourceSpanKind::CommunicationObservation
                 && span.kind != SourceSpanKind::ResourceObservation) {
            partition.protect(span.begin);
            partition.protect(span.end);
        }
    }

    std::set<SourceSpanId> named;
    std::size_t services = 0;
    for (auto & unit : partition.units()) {
        const auto & first = graph.spans()[unit.members.front()];
        const auto & last = graph.spans()[unit.members.back()];
        const auto owner = *first.owner_call;
        named.insert(unit.members.begin(), unit.members.end());
        auto begin_scope = scope(first.scope_call.value_or(owner));
        auto end_scope = scope(last.scope_call.value_or(owner));
        if (begin_scope.at("path").is_null() || end_scope.at("path").is_null())
            finding(gaps,
                    {
                        {      "reason", "native_cpu_service_scope_not_interpretable" },
                        {        "call",                        calls[owner].identity },
                        { "source_span",                         unit.members.front() }
            });
        cpu_services_[owner].push_back(
            { std::move(unit.service), first.begin, unit.end, graph.boundaries()[first.begin].lane, std::move(begin_scope), std::move(end_scope) });
        ++services;
    }
    for (const auto identity : candidates)
        if (!named.contains(identity))
            finding(gaps,
                    {
                        {      "reason",           "native_cpu_service_boundaries_not_named" },
                        {        "call", calls[*graph.spans()[identity].owner_call].identity },
                        { "source_span",                                            identity }
            });

    // This census starts with raw observation carriers, not extracted cuts or
    // template membership. Equal-time aliases on one native thread count once.
    using Coordinate = std::tuple<std::uint32_t, std::uint64_t, std::int64_t>;
    std::set<Coordinate> names, required;
    for (const auto & [point, name] : graph.cpu_boundaries()) {
        const auto & boundary = graph.boundaries()[point];
        const auto & lane = graph.lanes()[boundary.lane];
        names.emplace(lane.rank, lane.native_id, boundary.observed_time_ns);
    }
    std::map<std::string, std::size_t> owners;
    for (std::size_t index = 0; index < calls.size(); ++index) owners.emplace(calls[index].identity, serving_call(index));
    const auto owned = [&](const Json & record) {
        return !record.at("owner_call").is_null() && owners.contains(record.at("owner_call").get<std::string>()) && !record.value("observer", false);
    };
    std::size_t named_cuts = 0;
    const auto require = [&](const Json & record, const Json & time, std::string_view cut) {
        const Coordinate coordinate{ record.at("rank").get<std::uint32_t>(), record.at("tid").get<std::uint64_t>(), time.get<std::int64_t>() };
        if (!required.insert(coordinate).second) return;
        if (names.contains(coordinate)) ++named_cuts;
        else
            finding(gaps,
                    {
                        {  "reason", "native_cpu_cut_not_named" },
                        {     "cut",                        cut },
                        {    "rank",          record.at("rank") },
                        {     "tid",           record.at("tid") },
                        { "time_ns",                       time },
                        {  "source",        record.at("source") }
            });
    };
    for (const auto & interval : facts.at("intervals")) {
        if (interval.at("kind") != "cpu" || !owned(interval)) continue;
        const auto & name = interval.at("name").get_ref<const std::string &>();
        if (name.starts_with("markov/") && interval.at("scope_call") == interval.at("owner_call")) {
            require(interval, interval.at("start_ns"), "serving_entry");
            require(interval, interval.at("end_ns"), "serving_return");
        }
    }
    for (const auto & flow : facts.at("flows")) {
        if (flow.at("source_intervals").empty() || flow.at("target_intervals").empty()) continue;
        // NativeFlow connects carrier beginnings, not journal probes or API
        // envelopes. Retain every observed CPU-facing endpoint of that edge.
        for (const auto side : { "source_intervals", "target_intervals" })
            for (const auto & member : flow.at(side)) {
                const auto & interval = facts.at("intervals").at(member.get<std::size_t>());
                if (interval.at("kind") == "cpu" && owned(interval)) require(interval, interval.at("start_ns"), "native_flow");
            }
        if (flow.at("category") == "async_task_queue" && flow.at("source_intervals").size() == 1 && flow.at("target_intervals").size() == 1) {
            const auto & enqueue = facts.at("intervals").at(flow.at("source_intervals")[0].get<std::size_t>());
            const auto & worker = facts.at("intervals").at(flow.at("target_intervals")[0].get<std::size_t>());
            if (!owned(enqueue)) continue;
            if (worker.at("owner_call") != enqueue.at("owner_call") || worker.at("scope_call") != enqueue.at("scope_call"))
                finding(gaps,
                        {
                            {           "reason", "native_cpu_queue_scope_not_bound" },
                            { "enqueue_interval",                   enqueue.at("id") },
                            {  "worker_interval",                    worker.at("id") }
                });
            require(worker, worker.at("end_ns"), "worker_return");
        }
    }
    for (const auto & api : facts.at("native_api")) {
        // Workspace probes describe state. Their marker and effects have their
        // own coverage; probe clock reads are not additional runtime entries.
        if (!owned(api) || api.contains("profiler_control") || api.contains("workspace")) continue;
        if (api.at("copy_execution") == "synchronous_driver") {
            const auto & copy = api.at("copy_interval");
            require(api, copy.at("begin_ns"), "driver_copy_entry");
            require(api, copy.at("end_ns"), "driver_copy_return");
        }
        else if (native_host_wait(api.at("api").get<std::string>())) {
            const auto & wait = api.at("trace_interval");
            if (wait.is_null())
                finding(gaps,
                        {
                            { "reason", "native_cpu_wait_coordinate_not_observed" },
                            {    "api",                             api.at("api") },
                            { "source",                          api.at("source") }
                });
            else {
                require(api, wait.at("begin_ns"), "wait_entry");
                require(api, wait.at("end_ns"), "wait_return");
            }
        }
        // Other APIs qualify task/event identity in source extraction and the
        // native-submission census. Their enclosing carrier is not another cut.
    }
    cpu_coverage_ = {
        {                    "scope", "named serving, queue, wait, transfer and submission interfaces; private CPU call trees are not compared" },
        {                 "services",                                                                                                  services },
        {           "owned_segments",                                                                                         candidates.size() },
        {           "named_segments",                                                                                              named.size() },
        { "required_cut_coordinates",                                                                                           required.size() },
        {    "named_cut_coordinates",                                                                                                named_cuts },
        {                     "gaps",                                                                                           std::move(gaps) }
    };
}

std::span<const ObservedCpuService> SGLangStructureObservation::cpu_services(std::size_t call) const { return cpu_services_[call]; }

std::optional<std::vector<std::pair<std::size_t, BoundaryId>>> SGLangStructureObservation::incoming_request_ready(std::size_t call_index) const {
    const auto calls = source_.calls();
    const auto & call = calls[call_index];
    if (call.step != StepKind::Schedule) return std::vector<std::pair<std::size_t, BoundaryId>>{};
    if (call.input_supported != true || !call.input_ready_call || !calls[*call.input_ready_call].marker || serving_call(*call.input_ready_call) != call_index
        || call.request_ready_calls.size() != call.incoming_requests.size())
        return std::nullopt;
    std::map<std::size_t, BoundaryId> ready;
    for (const auto & [id, handler] : call.request_ready_calls) {
        const auto request = std::ranges::find(call.incoming_requests, id, &SourceIncomingRequest::id);
        const auto & native = calls[handler];
        if (request == call.incoming_requests.end() || !native.marker || native.parent != call.input_ready_call || serving_call(handler) != call_index)
            return std::nullopt;
        const auto slot = static_cast<std::size_t>(request - call.incoming_requests.begin());
        if (!ready.emplace(slot, source_.graph().spans()[*native.marker].end).second) return std::nullopt;
    }
    if (ready.size() != call.incoming_requests.size()) return std::nullopt;
    return std::vector<std::pair<std::size_t, BoundaryId>>(ready.begin(), ready.end());
}

std::size_t SGLangStructureObservation::serving_call(std::size_t call) const { return serving_calls_[call]; }

std::optional<ObservedBatchOutput> SGLangStructureObservation::output(std::size_t call) const {
    std::optional<ObservedBatchOutput> result;
    const auto calls = source_.calls();
    for (const auto index : regions_[call]) {
        const auto & region = calls[index];
        if (bindings_.region(*region.binding).semantic != "batch_output") continue;
        if (result || !region.marker) return std::nullopt;
        const auto & record = source_.facts().at("calls").at(region.identity).at("result");
        const auto found = record.find("output_work");
        if (found == record.end()) return std::nullopt;
        const auto & work = *found;
        const auto & before = work.at("before");
        const auto & after = work.at("after");
        if (before.size() != after.size()) return std::nullopt;
        const auto & marker = source_.graph().spans()[*region.marker];
        result = ObservedBatchOutput{ scope(index), marker.begin, marker.end, {} };
        if (!work.at("sender_enabled").get<bool>()) continue;
        for (std::size_t slot = 0; slot < before.size(); ++slot) {
            const auto & entry = before.at(slot);
            const auto & exit = after.at(slot);
            const auto previous = entry.at("send_token_offset").get<std::uint64_t>();
            const auto sent = exit.at("send_token_offset").get<std::uint64_t>();
            if (entry.at("request_id") != exit.at("request_id") || sent < previous) return std::nullopt;
            if (sent > previous)
                result->requests.push_back({ exit.at("request_id").get<std::string>(), sent - previous, sent, exit.at("finished").get<bool>() });
        }
    }
    return result;
}

void SGLangStructureObservation::read_collectives() {
    const auto calls = source_.calls();
    const auto & graph = source_.graph();
    std::set<std::size_t> logical_calls;
    std::map<std::size_t, std::size_t> implementation_slots;
    for (std::size_t index = 0; index < calls.size(); ++index) {
        if (calls[index].kind != SourceCallKind::NativeRegion || !calls[index].binding
            || bindings_.region(*calls[index].binding).semantic != "logical_collective")
            continue;
        logical_calls.insert(index);
        auto & implementations = collective_implementations_[serving_call(index)];
        implementation_slots.emplace(index, implementations.size());
        implementations.push_back({ index, {} });
    }
    const auto logical_owner = [&](std::optional<std::size_t> scope) {
        while (scope && calls[*scope].is_annotation()) {
            if (logical_calls.contains(*scope)) return scope;
            scope = calls[*scope].parent;
        }
        return std::optional<std::size_t>{};
    };

    // Only exact HCOM task identities belong to the logical transport. Stream
    // commands, CPU work and output layout keep their ordinary services.
    std::map<SourceSpanId, std::size_t> transport_owners;
    std::map<std::size_t, std::vector<SourceSpanId>> envelopes;
    for (const auto & [identity, collective] : graph.collectives()) {
        const auto owner = logical_owner(graph.spans()[identity].scope_call);
        if (!owner) continue;
        envelopes[*owner].push_back(identity);
        for (const auto & task : collective.tasks) transport_owners.emplace(task.span, *owner);
    }
    std::vector<std::optional<std::size_t>> owners(graph.boundaries().size());
    std::vector<std::optional<SourceSpanId>> device_spans(graph.boundaries().size());
    std::map<std::size_t, std::set<BoundaryId>> entries, completions;
    std::set<std::size_t> inconsistent_owners;
    for (SourceSpanId identity = 0; identity < graph.spans().size(); ++identity) {
        const auto & span = graph.spans()[identity];
        const auto & lane = graph.lanes()[graph.boundaries()[span.begin].lane];
        const auto owner = logical_owner(span.scope_call);
        if (lane.domain == SourceDomain::DeviceStream) { device_spans[span.begin] = device_spans[span.end] = identity; }
        if (!owner) continue;
        if (lane.domain != SourceDomain::DeviceStream || !transport_owners.contains(identity)) continue;
        if (transport_owners.at(identity) != *owner || span.owner_call != serving_call(*owner) || lane.rank != calls[*owner].rank)
            inconsistent_owners.insert(*owner);
        owners[span.begin] = owners[span.end] = owner;
        collective_implementations_[serving_call(*owner)][implementation_slots.at(*owner)].spans.push_back(identity);
        entries[*owner].insert(span.begin);
        completions[*owner].insert(span.end);
    }

    // A physical task join is internal only when every contributor is owned by
    // this implementation. Mixed joins remain external synchronization cuts.
    std::map<BoundaryId, std::vector<BoundaryId>> joins;
    std::set<BoundaryId> mixed_joins;
    std::vector<std::vector<std::size_t>> incoming(graph.boundaries().size());
    for (std::size_t index = 0; index < graph.edges().size(); ++index) {
        const auto & edge = graph.edges()[index];
        incoming[edge.target].push_back(index);
        if (edge.relation == SourceRelation::CompletionJoin && graph.lanes()[graph.boundaries()[edge.target].lane].domain == SourceDomain::DeviceTaskCompletion)
            joins[edge.target].push_back(edge.source);
        else mixed_joins.insert(edge.target);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto & [point, contributors] : joins) {
            if (owners[point] || mixed_joins.contains(point) || contributors.empty() || !owners[contributors.front()]) continue;
            const auto owner = owners[contributors.front()];
            if (!std::ranges::all_of(contributors, [&](auto source) { return owners[source] == owner; })) continue;
            owners[point] = owner;
            changed = true;
        }
    }
    std::map<std::size_t, DependencyPaths<BoundaryId>> implementation_paths;
    for (const auto & edge : graph.edges())
        if (owners[edge.source] && owners[edge.source] == owners[edge.target]) implementation_paths[*owners[edge.source]].connect(edge.source, edge.target);
    for (const auto index : logical_calls) {
        const auto & paths = implementation_paths[index];
        entries[index] = paths.frontier(std::move(entries[index]));
        completions[index] = paths.reversed().frontier(std::move(completions[index]));
    }

    // Every qualification below is proved in the unmodified observation DAG.
    // The logical ready gate does not claim that early helpers start at ready.
    const DirectedGraph native_paths(graph.boundaries().size(), graph.edges());
    if (!native_paths.acyclic()) {
        finding(collective_gaps_,
                {
                    { "reason", "collective_native_dependencies_contain_cycle" }
        });
        return;
    }
    std::vector<std::size_t> native_order(graph.boundaries().size());
    for (std::size_t index = 0; index < native_paths.order().size(); ++index) native_order[native_paths.order()[index]] = index;
    std::vector<std::uint32_t> visited(graph.boundaries().size());
    std::uint32_t generation = 0;
    std::map<std::pair<BoundaryId, BoundaryId>, bool> path_cache;
    const auto reaches = [&](BoundaryId source, BoundaryId target) {
        if (source == target) return true;
        if (native_order[source] >= native_order[target]) return false;
        const auto key = std::pair(source, target);
        if (const auto found = path_cache.find(key); found != path_cache.end()) return found->second;
        if (++generation == 0) {
            std::ranges::fill(visited, 0);
            ++generation;
        }
        std::vector<BoundaryId> pending{ source };
        visited[source] = generation;
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
            for (const auto index : native_paths.outgoing(pending[cursor])) {
                const auto next = graph.edges()[index].target;
                if (next == target) return path_cache.emplace(key, true).first->second;
                if (visited[next] == generation || native_order[next] > native_order[target]) continue;
                visited[next] = generation;
                pending.push_back(next);
            }
        }
        return path_cache.emplace(key, false).first->second;
    };
    const auto gate_precedes = [&](const auto & sources, const auto & targets) {
        return !sources.empty() && !targets.empty()
               && std::ranges::all_of(sources, [&](auto source) { return std::ranges::any_of(targets, [&](auto target) { return reaches(source, target); }); });
    };
    std::map<std::size_t, std::set<BoundaryId>> transport_inputs, transport_outputs;
    for (const auto & edge : graph.edges()) {
        if (owners[edge.source] == owners[edge.target]) continue;
        if (edge.relation == SourceRelation::NativeNotifySignal && owners[edge.source] && owners[edge.target]) continue;
        if (owners[edge.target]) transport_inputs[*owners[edge.target]].insert(edge.source);
        if (owners[edge.source]) transport_outputs[*owners[edge.source]].insert(edge.target);
    }
    std::map<std::size_t, ObservedCollective> candidates;
    std::map<std::size_t, std::vector<BoundaryId>> local_inputs;
    for (const auto index : logical_calls) {
        const auto & call = calls[index];
        const auto & binding = bindings_.region(*call.binding);
        const auto & record = source_.facts().at("calls").at(call.identity);
        const auto & metadata = record.at("metadata");
        if (!metadata.contains("group_ranks") || !metadata.at("group_ranks").is_array() || metadata.at("group_ranks").empty()) {
            finding(collective_gaps_,
                    {
                        { "reason", "collective_group_not_observed" },
                        {   "call",                   call.identity }
            });
            continue;
        }
        const auto ranks = metadata.at("group_ranks").get<std::vector<std::uint32_t>>();
        if (ranks.size() == 1 && ranks.front() == call.rank) {
            // An observed identity group does not declare a whole-device
            // service. Any actual implementation work stays ordinary work.
            collective_implementations_[serving_call(index)][implementation_slots.at(index)].spans.clear();
            continue;
        }
        const auto native = binding.read(record);
        const auto & input = native.inputs.at("input_");
        const auto & output = native.outputs.at("output");
        const bool gather = native.method == "all_gather";
        bool layout = input && output && input->shape.size() == 2 && input->dtype == output->dtype && input->shape[0] > 0 && input->shape[1] > 0
                      && native.parameters.at("physical_tp_size") == static_cast<std::int64_t>(ranks.size())
                      && std::set(ranks.begin(), ranks.end()).size() == ranks.size() && std::ranges::find(ranks, call.rank) != ranks.end();
        if (layout) {
            auto expected = input->shape;
            if (gather) {
                layout = native.call_parameters.at("dim") == -1;
                expected.back() *= static_cast<std::int64_t>(ranks.size());
            }
            else layout = native.method == "all_reduce";
            layout = layout && output->shape == expected && input->stride == std::vector<std::int64_t>{ input->shape[1], 1 }
                     && output->stride == std::vector<std::int64_t>{ output->shape[1], 1 };
        }
        const auto parent = call.parent;
        const auto found = envelopes.find(index);
        if (!layout || !parent || calls[*parent].kind != SourceCallKind::NativeRegion || !calls[*parent].binding || found == envelopes.end()
            || found->second.size() != 1 || !call.marker) {
            finding(collective_gaps_,
                    {
                        { "reason", "collective_native_layout_or_envelope_not_qualified" },
                        {   "call",                                        call.identity }
            });
            continue;
        }

        const auto envelope = found->second.front();
        const auto & collective = graph.collectives().at(envelope);
        const auto & host = graph.spans()[*call.marker];
        if (inconsistent_owners.contains(index) || collective.tasks.empty() || entries[index].empty() || completions[index].empty()
            || !std::ranges::all_of(collective.tasks, [&](const auto & task) { return owners[graph.spans()[task.span].begin] == index; })) {
            finding(collective_gaps_,
                    {
                        { "reason", "collective_native_frontier_not_observed" },
                        {   "call",                             call.identity }
            });
            continue;
        }
        // Input and launch readiness are read from actual edges entering the
        // transport, not the enclosing Python call or its CPU queue tail.
        const auto & input_points = transport_inputs[index];
        const auto & output_consumers = transport_outputs[index];
        bool seam = !input_points.empty() && !output_consumers.empty()
                    && collective.count == static_cast<std::uint64_t>(input->shape[0]) * static_cast<std::uint64_t>(input->shape[1])
                    && input->dtype == "torch.bfloat16" && collective.data_type == "BFP16";
        if (seam)
            for (const auto point : output_consumers)
                if (!gate_precedes(completions[index], std::vector{ point })) {
                    seam = false;
                    break;
                }
        if (!seam) {
            finding(collective_gaps_,
                    {
                        {  "reason", "collective_transport_seam_not_closed" },
                        {    "call",                          call.identity },
                        {  "inputs",                    input_points.size() },
                        { "outputs",                output_consumers.size() }
            });
            continue;
        }
        local_inputs[index].assign(input_points.begin(), input_points.end());
        auto logical_scope = scope(index);
        logical_scope["collective_position"] = std::string(bindings_.region(*calls[*parent].binding).semantic) + "/" + native.method;
        CollectiveWork work{
            gather ? "all_gather" : "all_reduce",
            ranks,
            { static_cast<std::uint64_t>(input->shape[0]), static_cast<std::uint64_t>(input->shape[1]) },
            input->dtype
        };
        candidates.emplace(index,
                           ObservedCollective{
                               index,
                               envelope,
                               std::move(work),
                               std::move(logical_scope),
                               host.begin,
                               host.end,
                               {     entries[index].begin(),     entries[index].end() },
                               { completions[index].begin(), completions[index].end() }
        });
    }

    const auto same_cohort = [&](std::size_t left, std::size_t right) {
        const auto & a = calls[serving_call(left)];
        const auto & b = calls[serving_call(right)];
        return a.batch_id && a.batch_id == b.batch_id && a.step == b.step && a.phase == b.phase
               && std::ranges::equal(a.requests, b.requests, [](const auto & x, const auto & y) {
                      return x.id == y.id && x.input_tokens == y.input_tokens && x.context_tokens == y.context_tokens && x.prompt_tokens == y.prompt_tokens
                             && x.generated_tokens == y.generated_tokens;
                  });
    };
    std::set<std::size_t> paired, qualified;
    std::map<std::size_t, std::vector<std::size_t>> group_members;
    for (const auto & [index, candidate] : candidates) {
        if (paired.contains(index)) continue;
        std::vector<std::size_t> members;
        std::set<std::uint32_t> ranks;
        for (const auto & [peer, item] : candidates)
            if (candidate.work == item.work && candidate.scope == item.scope && same_cohort(index, peer)) {
                members.push_back(peer);
                ranks.insert(calls[peer].rank);
            }
        if (members.empty()) members.push_back(index); // Missing cohort identity must still leave a concrete gap.
        paired.insert(members.begin(), members.end());
        const std::set<std::uint32_t> expected(candidate.work.ranks.begin(), candidate.work.ranks.end());
        if (members.size() != expected.size() || ranks != expected) {
            for (const auto member : members)
                finding(collective_gaps_,
                        {
                            { "reason", "collective_participant_interface_not_bound" },
                            {   "call",                       calls[member].identity }
                });
            continue;
        }
        std::vector<BoundaryId> ready;
        for (const auto member : members) {
            const auto & inputs = local_inputs.at(member);
            ready.insert(ready.end(), inputs.begin(), inputs.end());
        }
        std::ranges::sort(ready);
        ready.erase(std::unique(ready.begin(), ready.end()), ready.end());
        // This proof precedes any interface substitution. max(ready) is a
        // logical gate, not the start of every internal physical interval.
        if (!std::ranges::all_of(members, [&](auto member) { return gate_precedes(ready, candidates.at(member).completions); })) {
            for (const auto member : members)
                finding(collective_gaps_,
                        {
                            { "reason", "collective_group_ready_not_observed_before_local_done" },
                            {   "call",                                  calls[member].identity }
                });
            continue;
        }
        for (const auto member : members) {
            candidates.at(member).entries = ready;
            group_members.emplace(member, members);
            qualified.insert(member);
        }
    }

    std::set<BoundaryId> named_points;
    for (const auto & call : calls) {
        if (call.marker) {
            named_points.insert(graph.spans()[*call.marker].begin);
            named_points.insert(graph.spans()[*call.marker].end);
        }
        if (call.device_done) named_points.insert(*call.device_done);
    }
    for (const auto & block : source_.subgraphs().blocks)
        for (const auto & [kind, point] : block.ports) named_points.insert(point);

    // An external wait carries dependencies, not an extra work item. Project
    // through it to independent services or named ports using original edges.
    // A task join is an AND gate, so retain every contributor on its input.
    const auto source_gate = [&](BoundaryId point) {
        std::set<BoundaryId> result, seen;
        std::vector<BoundaryId> pending{ point };
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
            const auto current = pending[cursor];
            if (!seen.insert(current).second) continue;
            if (owners[current] && qualified.contains(*owners[current])) {
                const auto & points = candidates.at(*owners[current]).completions;
                result.insert(points.begin(), points.end());
            }
            else if (const auto found = joins.find(current); found != joins.end() && !mixed_joins.contains(current)) {
                for (const auto contributor : found->second) pending.push_back(contributor);
            }
            else if (device_spans[current] && graph.spans()[*device_spans[current]].kind == SourceSpanKind::DeviceWait) {
                for (const auto edge_index : incoming[current]) pending.push_back(graph.edges()[edge_index].source);
            }
            else result.insert(current);
        }
        return std::vector<BoundaryId>(result.begin(), result.end());
    };
    const auto target_gates = [&](BoundaryId point) {
        std::set<std::vector<BoundaryId>> result;
        std::set<BoundaryId> seen;
        std::vector<BoundaryId> pending{ point };
        bool observed = true;
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
            const auto current = pending[cursor];
            if (!seen.insert(current).second) continue;
            if (owners[current] && qualified.contains(*owners[current])) result.insert(candidates.at(*owners[current]).entries);
            else if (named_points.contains(current) || (device_spans[current] && graph.spans()[*device_spans[current]].kind != SourceSpanKind::DeviceWait))
                result.insert({ current });
            else {
                const auto next = native_paths.outgoing(current);
                if (next.empty()) observed = false;
                for (const auto edge_index : next) pending.push_back(graph.edges()[edge_index].target);
            }
        }
        if (!observed) result.clear();
        return result;
    };
    // Removing an interface can expose a real external consumer previously
    // represented by that interface. Recheck until no qualification changes.
    changed = true;
    while (changed) {
        changed = false;
        std::map<std::size_t, Json> failures;
        for (const auto & edge : graph.edges()) {
            const auto source = owners[edge.source], target = owners[edge.target];
            const bool source_bound = source && qualified.contains(*source), target_bound = target && qualified.contains(*target);
            if ((!source_bound && !target_bound) || source == target) continue;
            if (edge.relation == SourceRelation::NativeNotifySignal && source_bound && target_bound && calls[*source].rank != calls[*target].rank
                && group_members.at(*source) == group_members.at(*target))
                continue;
            const auto before = source_gate(edge.source);
            const auto after = target_gates(edge.target);
            const auto failed = std::ranges::find_if(after, [&](const auto & points) { return !gate_precedes(before, points); });
            if (!before.empty() && !after.empty() && failed == after.end()) continue;
            Json detail{
                {           "reason", after.empty() ? "collective_external_consumer_not_observed" : "collective_external_constraint_not_projectable" },
                {         "relation",                                                                      SourceGraph::relation_name(edge.relation) },
                {           "source",                                                                                                    edge.source },
                {           "target",                                                                                                    edge.target },
                { "source_reference",                                           edge.source_reference ? Json(*edge.source_reference) : Json(nullptr) },
                { "source_gate_size",                                                                                                  before.size() },
                { "target_gate_size",                                                                     failed == after.end() ? 0 : failed->size() }
            };
            for (const auto owner : { source, target })
                if (owner && qualified.contains(*owner)) failures.try_emplace(*owner, detail);
        }
        for (const auto & [owner, detail] : failures)
            for (const auto member : group_members.at(owner)) {
                if (!qualified.erase(member)) continue;
                auto gap = detail;
                gap["call"] = calls[member].identity;
                gap["interface_owner"] = calls[owner].identity;
                finding(collective_gaps_, gap);
                changed = true;
            }
    }
    for (auto & [index, candidate] : candidates) {
        if (!qualified.contains(index)) continue;
        collective_implementations_[serving_call(index)][implementation_slots.at(index)].qualified = true;
        collectives_[serving_call(index)].push_back(std::move(candidate));
    }
}

std::vector<ObservedCollective> SGLangStructureObservation::collectives(std::size_t call) const { return collectives_[call]; }

std::vector<ObservedCollectiveImplementation> SGLangStructureObservation::collective_implementations(std::size_t call) const {
    return collective_implementations_[call];
}

std::optional<DeviceCommandWork> SGLangStructureObservation::command(SourceSpanId span) const {
    const auto interval = source_.graph().spans()[span].source_interval;
    return interval ? AscendSourceExtractor::device_command(source_.facts().at("intervals").at(*interval)) : std::nullopt;
}

Json SGLangStructureObservation::scope(std::size_t call) const {
    const auto calls = source_.calls();
    const auto owner = serving_call(call);
    std::vector<std::string> path;
    bool known = true;
    Json layer = nullptr, slot = nullptr, input_slot = nullptr;
    while (call != owner) {
        const auto & current = calls[call];
        if (current.kind == SourceCallKind::NativeRegion && current.binding) {
            const auto semantic = bindings_.region(*current.binding).semantic;
            path.emplace_back(semantic);
            const auto & metadata = source_.facts().at("calls").at(current.identity).at("metadata");
            const auto & parameters = metadata.at("module_parameters");
            if (layer.is_null() && parameters.contains("layer_index")) layer = parameters.at("layer_index");
            if (!metadata.at("request_id").is_null()) {
                const auto id = metadata.at("request_id").get<std::string>();
                if (semantic == "input_request") {
                    if (input_slot.is_null()) {
                        const auto request = std::ranges::find(calls[owner].incoming_requests, id, &SourceIncomingRequest::id);
                        if (request != calls[owner].incoming_requests.end()) input_slot = request - calls[owner].incoming_requests.begin();
                    }
                }
                else if (slot.is_null()) {
                    const auto request = std::ranges::find(calls[owner].requests, id, &SourceRequest::id);
                    if (request != calls[owner].requests.end()) slot = request - calls[owner].requests.begin();
                }
            }
        }
        else if (current.kind == SourceCallKind::NativeRegion) known = false;
        call = current.parent.value();
    }
    std::ranges::reverse(path);
    return {
        {      "binding",                     "source_scope" },
        {         "path", known ? Json(path) : Json(nullptr) },
        { "request_slot",                               slot },
        {   "input_slot",                         input_slot },
        {  "model_layer",                              layer }
    };
}

Json SGLangStructureObservation::compare_workspace(std::size_t call_index, const Json & resource_nodes) const {
    ObservedState state;
    const auto & uses = workspace_uses_[call_index];
    if (resource_nodes.empty() && uses.empty()) return std::move(state.data);
    if (resource_nodes.empty() || uses.empty()) {
        state.unknown(resource_nodes.empty() ? "workspace.target_effects" : "workspace.native_effects");
        return std::move(state.data);
    }

    // Semantic position joins independent executions. Source IDs and clocks do
    // not identify a target use, and an owner prefix is not a complete scope.
    const auto semantic_key = [](const Json & value) -> std::optional<Json> {
        if (value.is_null() || value.at("path").is_null() || (value.at("binding") != "source_scope" && value.at("binding") != "target_scope"))
            return std::nullopt;
        return Json::array({ value.at("path"), value.at("model_layer"), value.at("request_slot") });
    };
    std::map<Json, std::vector<const Json *>> targets;
    std::map<Json, std::vector<std::size_t>> native;
    bool scopes_known = true;
    for (const auto & node : resource_nodes) {
        const auto key = semantic_key(node.at("logical_scope"));
        if (key) targets[*key].push_back(&node);
        else {
            scopes_known = false;
            state.unknown("workspace.target_scope");
        }
    }
    for (const auto index : uses) {
        const auto & record = source_.facts().at("native_api")[index];
        if (record.at("source_interval").is_null()) {
            scopes_known = false;
            state.unknown("workspace.native_marker");
            continue;
        }
        const auto & marker = source_.graph().spans()[record.at("source_interval").get<std::size_t>()];
        const auto key = marker.scope_call ? semantic_key(scope(*marker.scope_call)) : std::nullopt;
        if (marker.kind == SourceSpanKind::ResourceObservation && marker.owner_call == call_index && key) native[*key].push_back(index);
        else {
            scopes_known = false;
            state.unknown("workspace.native_scope");
        }
    }
    if (!scopes_known) return std::move(state.data);

    state.compare("workspace.uses", resource_nodes.size(), uses.size());
    for (const auto & [key, entries] : targets) {
        const auto found = native.find(key);
        const auto count = found == native.end() ? 0 : found->second.size();
        const auto field = "workspace[" + key.dump() + "]";
        state.compare(field + ".uses", entries.size(), count);
        if (entries.size() != 1 || count != 1) {
            state.unknown(field + ".unique_use");
            continue;
        }
        const auto & node = *entries.front();
        const auto & effect = node.at("workspace");
        const auto api = found->second.front();
        const auto & record = source_.facts().at("native_api")[api];
        if (effect.is_null() || !record.contains("workspace_allocation")) {
            state.unknown(field + ".effect");
            continue;
        }
        const auto & transition = record.at("workspace");
        const auto & allocation = record.at("workspace_allocation");
        const auto pool = native_workspace_pool(record);
        const auto window = workspace_windows_.find({ record.at("rank").get<std::uint32_t>(), record.at("pid").get<std::uint64_t>() });
        if (window == workspace_windows_.end() || record.at("result") != 0 || transition.at("policy") != "cached") {
            state.unknown(field + ".native_window");
            continue;
        }

        auto target_pool = effect.at("pool");
        if (!target_pool.at("role").is_null()) target_pool.erase("native_id");
        const auto [alias, first] = workspace_pools_.try_emplace(target_pool, pool);
        const bool exclusive = std::ranges::none_of(workspace_pools_, [&](const auto & item) { return item.first != target_pool && item.second == pool; });
        const bool bound = alias->second == pool && exclusive && target_pool.at("domain") == "device_stream" && target_pool.at("rank") == record.at("rank");
        state.compare(field + ".pool_alias", true, bound);

        // Complete snapshots establish empty or retained pools. Dynamic capacity
        // effects are compared directly; their ordering remains in the common DAG.
        const auto snapshot_capacity = [&](std::string_view boundary) {
            const auto & entries = window->second->at(boundary);
            const auto match = std::ranges::find_if(entries, [&](const Json & item) {
                return item.at("device") == transition.at("device") && item.at("stream_handle") == record.at("stream_handle");
            });
            return match == entries.end() ? Json(0) : match->at("capacity_bytes");
        };
        bool agrees = bound;
        if (first) {
            const auto capacity = snapshot_capacity("initial");
            state.compare(field + ".initial_capacity", effect.at("capacity_before_bytes"), capacity);
            agrees = agrees && effect.at("capacity_before_bytes") == capacity;
        }
        if (workspace_last_.at(pool) == api) {
            const auto capacity = snapshot_capacity("final");
            state.compare(field + ".final_capacity", effect.at("capacity_after_bytes"), capacity);
            agrees = agrees && effect.at("capacity_after_bytes") == capacity;
        }
        const auto compare_effect = [&](std::string_view name, const Json & observed) {
            const auto & target = effect.at(name);
            if (target.is_null()) {
                state.unknown(field + "." + std::string(name));
                agrees = false;
            }
            else {
                state.compare(field + "." + std::string(name), target, observed);
                agrees = agrees && target == observed;
            }
        };
        compare_effect("demand_bytes", transition.at("requested_bytes"));
        compare_effect("capacity_before_bytes", transition.at("capacity_before_bytes"));
        compare_effect("capacity_after_bytes", transition.at("capacity_after_bytes"));
        compare_effect("branch", allocation.at("branch"));
        compare_effect("requires_device_synchronization", allocation.at("requires_device_synchronization"));
        if (allocation.at("branch") != "reuse" || effect.at("branch") != "reuse") {
            state.unknown(field + ".transition_domain");
            agrees = false;
        }
        if (agrees) {
            const auto & marker = source_.graph().spans()[record.at("source_interval").get<std::size_t>()];
            state.data["resource_boundaries"].push_back({
                { "target_begin", node.at("begin") },
                {   "target_end",   node.at("end") },
                { "native_begin",     marker.begin },
                {   "native_end",       marker.end }
            });
        }
    }
    for (const auto & [key, entries] : native)
        if (!targets.contains(key)) state.unknown("workspace.native_use_not_in_target:" + key.dump());
    return std::move(state.data);
}

Json SGLangStructureObservation::compare_state(std::size_t call_index, const Json & invocation, const Json & resource_nodes) const {
    const auto calls = source_.calls();
    const auto & call = calls[call_index];
    const auto & facts = source_.facts().at("calls");
    const auto & result = facts.at(call.identity).at("result");
    const auto & requests = invocation.at("requests");
    ObservedState state{ compare_workspace(call_index, resource_nodes) };

    if (call.step == StepKind::Schedule) {
        if (call.input_supported == true) {
            auto incoming = Json::array();
            for (const auto & request : call.incoming_requests)
                incoming.push_back({
                    {            "id",            request.id },
                    { "prompt_tokens", request.prompt_tokens },
                    { "output_tokens", request.output_tokens }
                });
            auto target = Json::array();
            for (const auto & request : invocation.at("incoming_requests"))
                target.push_back({
                    {            "id",            request.at("id") },
                    { "prompt_tokens", request.at("prompt_tokens") },
                    { "output_tokens", request.at("output_tokens") }
                });
            state.compare("incoming_requests", target, incoming);
        }
        else state.unknown("input_reception");
        auto filters = Json::array();
        for (const auto & filter : call.batch_filters) {
            auto retained = Json::array();
            for (const auto & id : filter.after) {
                const auto member = std::ranges::find(filter.before, id);
                retained.push_back(member == filter.before.end() ? Json(nullptr) : Json(member - filter.before.begin()));
            }
            filters.push_back({
                {      "input_count", filter.before.size() },
                { "retained_indices",             retained }
            });
        }
        state.compare("batch_filters", invocation.at("batch_filters"), filters);
    }

    std::set<std::string> observed;
    std::size_t budget_index = 0;
    Json merge = nullptr;

    // Per-request frees end logical lifetimes. A group flush commits their
    // combined allocator work without releasing those requests a second time.
    struct ReleaseGroup {
        std::uint64_t tokens = 0;
        std::size_t members = 0;
        bool ended = false;
        bool observed = true;
        std::vector<std::uint64_t> flushes;
    };
    std::vector<ReleaseGroup> release_groups;
    std::optional<std::size_t> active_release_group;
    std::map<std::size_t, std::size_t> release_group_ends;
    std::set<std::string> released_requests;

    for (const auto index : regions_[call_index]) {
        const auto region = bindings_.region(*calls[index].binding).read(facts.at(calls[index].identity));
        const auto & semantic = region.semantic;
        const auto & parameters = region.call_parameters;
        observed.insert(semantic);
        if (call.step == StepKind::Schedule && semantic == "batch_transition") {
            state.compare(
                "batch_transition",
                invocation.at("batch_transition"),
                {
                    {         "running_requests",                                                                      parameters.at("running_requests") },
                    { "pending_prefill_requests", parameters.at("has_pending_prefill") ? Json(parameters.at("pending_prefill_requests")) : Json(nullptr) },
                    {        "admission_blocked",                                                                    parameters.at("batch_is_full") != 0 }
            });
        }
        if (call.step == StepKind::Schedule && semantic == "admission_prefix") {
            state.compare("admission_entry",
                          invocation.at("admission_entry"),
                          {
                              {  "waiting_requests",   parameters.at("waiting_requests") },
                              {  "running_requests",   parameters.at("running_requests") },
                              { "admission_blocked", parameters.at("batch_is_full") != 0 }
            });
            if (!region.result.contains("admission_work")) {
                state.unknown("admission_work");
                continue;
            }
            const auto & work = region.result.at("admission_work");
            for (const auto field : { "candidate_checks", "request_checks", "queue_compaction_items" })
                state.compare(std::string("admission_work.") + field, invocation.at("admission_work").at(field), work.at(field));

            const auto stop = work.at("native_stop").get<std::string>();
            const auto common_stop = bindings_.admission_stop(region, request_capacity_);
            if (common_stop) state.compare("admission_work.stop", invocation.at("admission_work").at("stop"), AdmissionWork::stop_name(*common_stop));
            else state.unknown("admission_work.stop:" + stop);
        }
        if (call.step == StepKind::Schedule && semantic == "admission_budget") {
            const auto & horizons = invocation.at("admission_work").at("request_horizon_checks");
            const auto admitted = invocation.at("phase") == "prefill" ? requests.size() : 0;
            // An accepted position identifies a real member of this matched
            // batch. Rejected candidates have no request identity in these facts.
            if (budget_index < horizons.size() && budget_index < admitted && region.result.contains("horizon_checks")
                && region.result.value("candidate_accepted", false) && parameters.at("accepted_requests") == static_cast<std::int64_t>(budget_index)
                && parameters.at("input_tokens") == requests.at(budget_index).at("input_tokens").get<std::int64_t>()) {
                const auto field = "admission_budget[" + std::to_string(budget_index) + "]";
                state.compare(field + ".horizon_checks", horizons.at(budget_index), region.result.at("horizon_checks"));
                state.compare(field + ".accepted_requests", std::min(budget_index, admitted), parameters.at("accepted_requests"));
                state.compare(field + ".candidate_accepted", budget_index < admitted, region.result.at("candidate_accepted"));
                const auto & stop = invocation.at("admission_work").at("stop");
                if (region.result.contains("native_result") && (budget_index + 1 < horizons.size() || stop == "queue_exhausted" || stop == "request_capacity"))
                    state.compare(field + ".native_result", "CONTINUE", region.result.at("native_result"));
                else state.unknown(field + ".native_stop");
            }
            else state.unknown("admission_budget.candidate_correspondence");
            ++budget_index;
        }
        if (call.step == StepKind::Schedule && semantic == "prefill_statistics")
            state.compare("admission_work.pending_statistics_items",
                          invocation.at("admission_work").at("pending_statistics_items"),
                          parameters.at("waiting_requests"));
        if (call.step == StepKind::Schedule && semantic == "batch_merge") {
            merge = {
                {  "left_count",  parameters.at("left_requests") },
                { "right_count", parameters.at("right_requests") }
            };
            // The pending-prefill filter precedes merge; any running update
            // filter follows it. Ordered memberships are checked above.
            const auto prefix_count = invocation.at("batch_transition").at("pending_prefill_requests").is_null() ? 0U : 1U;
            state.compare("batch_merge.filter_position", prefix_count, parameters.at("filter_prefix_count"));
        }
        if (call.step == StepKind::Prepare && semantic == "kv_allocation") {
            const auto & free = region.inputs.at("free_pages");
            if (free) state.compare("free_kv_pages", invocation.at("free_kv_pages"), free->shape.at(0));
            else state.unknown("free_kv_pages");

            const auto & lengths = region.cpu_values.at("seq_lens_cpu");
            const auto page = region.parameters.at("page_size");
            state.compare("kv_sequence_count", requests.size(), lengths.size());
            if (page <= 0 || lengths.size() != requests.size()) {
                state.unknown("kv_page_demand");
                continue;
            }
            const auto prefixes = region.cpu_values.find("prefix_lens_cpu");
            for (std::size_t slot = 0; slot < requests.size(); ++slot) {
                const auto & request = requests.at(slot);
                const auto prefix = prefixes == region.cpu_values.end() ? lengths[slot] - 1 : prefixes->second.at(slot);
                const auto target_prefix = request.at("context_tokens").get<std::int64_t>();
                const auto target_length = target_prefix + request.at("input_tokens").get<std::int64_t>();
                const auto field = "kv[" + request.at("id").get<std::string>() + "]";
                state.compare(field + ".prefix_tokens", target_prefix, prefix);
                state.compare(field + ".sequence_tokens", target_length, lengths[slot]);
                state.compare(field + ".pages", request.at("kv_pages").size(), (lengths[slot] + page - 1) / page);
                state.compare(field + ".new_pages",
                              (target_length + page - 1) / page - (target_prefix + page - 1) / page,
                              (lengths[slot] + page - 1) / page - (prefix + page - 1) / page);
            }
        }
        if (call.step == StepKind::Publish && semantic == "kv_release_group") {
            const auto & method = bindings_.region(*calls[index].binding).method;
            if (method == "free_group_begin") {
                if (active_release_group) state.unknown("kv_release_group.nested_begin");
                release_groups.emplace_back();
                active_release_group = release_groups.size() - 1;
            }
            else if (method == "free_group_end") {
                if (active_release_group) {
                    release_groups[*active_release_group].ended = true;
                    release_group_ends.emplace(index, *active_release_group);
                    active_release_group.reset();
                }
                else state.unknown("kv_release_group.begin");
            }
        }
        if (call.step == StepKind::Publish && semantic == "kv_release") {
            const auto slot = scope(index).at("request_slot");
            const auto & released = region.inputs.at("free_index");

            // The flush is identified by its real group-end ancestor, never
            // merely by the allocator's is_not_in_free_group flag.
            auto ancestor = calls[index].parent;
            while (ancestor && *ancestor != call_index && !release_group_ends.contains(*ancestor)) ancestor = calls[*ancestor].parent;
            if (ancestor && release_group_ends.contains(*ancestor)) {
                auto & group = release_groups[release_group_ends.at(*ancestor)];
                if (released) group.flushes.push_back(released->shape.at(0));
                else {
                    group.observed = false;
                    state.unknown("kv_release_group.flush_tokens");
                }
            }
            else {
                if (!region.parameters.at("is_not_in_free_group")) {
                    if (active_release_group) {
                        auto & group = release_groups[*active_release_group];
                        if (released) {
                            group.tokens += released->shape.at(0);
                            ++group.members;
                        }
                        else group.observed = false;
                    }
                    else state.unknown("kv_release_group.member");
                }
                if (!slot.is_null() && released) {
                    const auto & request = requests.at(slot.get<std::size_t>());
                    state.compare("kv_release.once:" + request.at("id").get<std::string>(),
                                  true,
                                  released_requests.insert(request.at("id").get<std::string>()).second,
                                  true);
                    state.compare("kv_release.tokens",
                                  request.at("context_tokens").get<std::uint64_t>() + request.at("input_tokens").get<std::uint64_t>(),
                                  released->shape.at(0),
                                  true);
                    state.compare("kv_release.finished", request.at("finishes"), true, true);
                }
                else state.unknown("kv_release.request_and_pages");
            }
        }
    }

    if (call.step == StepKind::Schedule) {
        for (const auto semantic : { "batch_transition", "admission_prefix" })
            if (!observed.contains(semantic)) state.unknown(semantic);
        state.compare("batch_merge", invocation.at("batch_merge"), merge);
        state.compare("admission_budget.calls", invocation.at("admission_work").at("request_checks"), budget_index);
        if (!observed.contains("prefill_statistics") && invocation.at("admission_work").at("pending_statistics_items") != 0)
            state.unknown("admission_work.pending_statistics_items");
    }
    if (call.step == StepKind::Prepare) {
        if (!observed.contains("kv_allocation")) state.unknown("kv_allocation");
        if (!result.contains("request_pool_slots")) state.unknown("request_pool_slots");
        else {
            const auto & slots = result.at("request_pool_slots");
            auto expected_ids = Json::array(), observed_ids = Json::array();
            for (const auto & request : requests) expected_ids.push_back(request.at("id"));
            for (const auto & entry : slots) observed_ids.push_back(entry.at("request_id"));
            state.compare("request_pool_slots.members", expected_ids, observed_ids);
            for (const auto & request : requests) {
                const auto id = request.at("id").get<std::string>();
                const auto native = std::ranges::find_if(slots, [&](const Json & entry) { return entry.at("request_id") == id; });
                if (native == slots.end()) {
                    state.unknown("request_pool_slot:" + id);
                    continue;
                }
                const auto key = std::pair{ call.rank, id };
                const auto pair = std::pair{ request.at("pool_slot").get<std::uint32_t>(), native->at("pool_slot").get<std::uint32_t>() };
                bool exclusive = true;
                for (const auto & [other, binding] : live_slots_)
                    if (other.first == call.rank && other.second != id && (binding.first == pair.first || binding.second == pair.second)) exclusive = false;
                state.compare("slot_correspondence.exclusive:" + id, true, exclusive);
                const auto prior = live_slots_.find(key);
                if (prior != live_slots_.end()) state.compare("slot_correspondence.retained:" + id, prior->second, pair);
                else if (call.phase == BatchPhase::Decode) state.unknown("slot_correspondence.initial_allocation:" + id);
                live_slots_.insert_or_assign(key, pair);
            }
        }
    }
    if (call.step == StepKind::Publish) {
        for (const auto & group : release_groups) {
            if (!group.ended) {
                state.unknown("kv_release_group.end");
                continue;
            }
            if (!group.observed) continue;
            state.compare("kv_release_group.flush_calls", group.members == 0 ? 0 : 1, group.flushes.size());
            if (group.flushes.size() == 1) state.compare("kv_release_group.tokens", group.tokens, group.flushes.front());
        }

        auto expected_ids = Json::array(), observed_ids = Json::array();
        for (const auto & request : requests) expected_ids.push_back(request.at("id"));
        for (const auto & request : result.at("requests")) observed_ids.push_back(request.at("id"));
        state.compare("result_processed.members", expected_ids, observed_ids, true);
        for (const auto & request : requests) {
            const auto id = request.at("id").get<std::string>();
            const auto native = std::ranges::find_if(result.at("requests"), [&](const Json & entry) { return entry.at("id") == id; });
            if (native == result.at("requests").end()) {
                state.unknown("result_processed:" + id);
                continue;
            }
            state.compare("result_processed.generated_tokens:" + id,
                          request.at("generated_tokens").get<std::uint64_t>() + 1,
                          native->at("generated_tokens"),
                          true);
            state.compare("result_processed.finished:" + id, request.at("finishes"), native->at("finished"), true);
            if (native->at("finished").get<bool>()) live_slots_.erase({ call.rank, id });
            if (request.at("finishes").get<bool>() && !released_requests.contains(id)) state.unknown("kv_release_lifetime:" + id);
        }
    }
    return std::move(state.data);
}

Json SGLangStructureObservation::coverage() const {
    const auto & graph = source_.graph();
    const auto & facts = source_.facts();
    Json gaps{
        {     "total", source_.gaps().size() + collective_gaps_.at("total").get<std::size_t>() + cpu_coverage_.at("gaps").at("total").get<std::size_t>() },
        {     "shown",                                                                                                                                 0 },
        { "by_reason",                                                                                                                    Json::object() },
        {  "examples",                                                                                                                     Json::array() }
    };
    std::set<std::uint32_t> worker_ranks, control_ranks;
    std::set<std::pair<std::uint32_t, SGLangBindings::WorkerControlQualification>> control_gaps;
    for (const auto & call : source_.calls()) {
        if (call.is_annotation()) continue;
        worker_ranks.insert(call.rank);
        if (call.step != StepKind::Schedule) continue;
        control_ranks.insert(call.rank);
        const auto qualification = bindings_.qualify_worker_controls(facts.at("calls").at(call.identity).at("metadata"));
        if (qualification != SGLangBindings::WorkerControlQualification::Supported) control_gaps.emplace(call.rank, qualification);
    }
    for (const auto rank : worker_ranks)
        if (!control_ranks.contains(rank)) control_gaps.emplace(rank, SGLangBindings::WorkerControlQualification::NotObserved);
    // Qualify the same effective worker policy independently of template production.
    // Repeated Schedule records share one gap per worker and qualification.
    for (const auto & [rank, qualification] : control_gaps) {
        const bool missing = qualification == SGLangBindings::WorkerControlQualification::NotObserved;
        finding(gaps,
                {
                    {   "kind",                                                               missing ? "observation" : "rule" },
                    { "reason", missing ? "native_worker_controls_not_observed" : "native_worker_control_path_not_implemented" },
                    {   "rank",                                                                                           rank }
        });
    }
    for (const auto & source_gap : source_.gaps()) {
        auto & count = gaps["by_reason"][source_gap.at("reason").get<std::string>()];
        count = count.is_null() ? 1 : count.get<std::size_t>() + 1;
    }
    for (const auto * report : { &collective_gaps_, &cpu_coverage_.at("gaps") })
        if (report->contains("by_reason"))
            for (const auto & [reason, count] : report->at("by_reason").items()) {
                auto & total = gaps["by_reason"][reason];
                total = (total.is_null() ? 0 : total.get<std::size_t>()) + count.get<std::size_t>();
            }
    for (const auto & source_gap : source_.gaps()) {
        if (gaps.at("examples").size() == 5) break;
        gaps["examples"].push_back(source_gap);
    }
    for (const auto * report : { &collective_gaps_, &cpu_coverage_.at("gaps") })
        for (const auto & detail : report->at("examples")) {
            if (gaps.at("examples").size() == 5) break;
            gaps["examples"].push_back(detail);
        }
    gaps["shown"] = gaps.at("examples").size();
    std::map<std::string, std::size_t> owners;
    for (std::size_t index = 0; index < source_.calls().size(); ++index) owners.emplace(source_.calls()[index].identity, serving_call(index));
    std::set<SourceSpanId> members;
    std::set<std::size_t> retained_edges;
    for (const auto & block : source_.subgraphs().blocks) {
        members.insert(block.spans.begin(), block.spans.end());
        retained_edges.insert(block.edges.begin(), block.edges.end());
    }
    for (const auto & crossing : source_.subgraphs().crossings) retained_edges.insert(crossing.edge);
    std::set<std::size_t> retained_intervals, retained_apis;
    for (const auto span_id : members) {
        const auto & span = graph.spans()[span_id];
        if (span.source_interval) retained_intervals.insert(*span.source_interval);
        if (span.source_api) retained_apis.insert(*span.source_api);
        const auto transfer = graph.transfers().find(span_id);
        if (transfer != graph.transfers().end() && transfer->second.source_api) retained_apis.insert(*transfer->second.source_api);
    }
    for (const auto index : retained_edges) {
        const auto & edge = graph.edges()[index];
        if (edge.source_reference
            && (edge.relation == SourceRelation::NativeHostWait || edge.relation == SourceRelation::NativeTaskDependency
                || edge.relation == SourceRelation::NativeEventSignal))
            retained_apis.insert(*edge.source_reference);
    }
    std::map<std::tuple<std::uint32_t, std::uint64_t, std::uint64_t>, std::vector<std::size_t>> hardware_tasks;
    std::size_t hardware = 0, covered_hardware = 0, submissions = 0, covered_submissions = 0;
    for (const auto & interval : facts.at("intervals")) {
        if (interval.at("process") != "Ascend Hardware" || interval.at("owner_call").is_null() || !owners.contains(interval.at("owner_call").get<std::string>())
            || interval.value("observer", false))
            continue;
        ++hardware;
        const auto identity = interval.at("id").get<std::size_t>();
        const auto & arguments = interval.at("arguments");
        hardware_tasks[{ interval.at("rank"), arguments.at("Physic Stream Id"), arguments.at("Task Id") }].push_back(identity);
        if (retained_intervals.contains(identity)) ++covered_hardware;
        else
            finding(gaps,
                    {
                        {       "reason",                                       "owned_hardware_interval_missing_from_cuts" },
                        {     "interval",                                                                          identity },
                        { "serving_call", source_.calls()[owners.at(interval.at("owner_call").get<std::string>())].identity },
                        {       "source",                                                             interval.at("source") }
            });
    }
    for (std::size_t index = 0; index < facts.at("native_api").size(); ++index) {
        const auto & api = facts.at("native_api").at(index);
        if (api.at("owner_call").is_null() || !owners.contains(api.at("owner_call").get<std::string>()) || api.value("observer", false)
            || api.contains("profiler_control"))
            continue;
        const auto name = api.at("api").get<std::string>();
        const bool task = name == "rtEventRecord" || name == "rtStreamWaitEvent" || name == "rtMemcpyAsync" || name == "rtGeneralCtrl";
        const bool wait = native_host_wait(name);
        if (!task && !wait) continue;
        ++submissions;
        bool covered = api.at("result") == 0
                       && (retained_apis.contains(index)
                           || (!api.at("hardware_intervals").empty() && std::ranges::all_of(api.at("hardware_intervals"), [&](const Json & member) {
                                  return retained_intervals.contains(member.get<std::size_t>());
                              })));
        if (!covered && name == "rtGeneralCtrl" && api.at("result") == 0 && !api.at("hardware_task_id").is_null()) {
            const auto found = hardware_tasks.find({ api.at("rank"), api.at("submitted_stream_id"), api.at("hardware_task_id") });
            covered = found != hardware_tasks.end() && std::ranges::all_of(found->second, [&](auto interval) { return retained_intervals.contains(interval); });
        }
        if (covered) ++covered_submissions;
        else
            finding(gaps,
                    {
                        {       "reason",                               "native_submission_not_independently_retained" },
                        {          "api",                                                                         name },
                        { "serving_call", source_.calls()[owners.at(api.at("owner_call").get<std::string>())].identity },
                        {       "source",                                                             api.at("source") }
            });
    }
    auto cpu = cpu_coverage_;
    cpu.erase("gaps");
    std::size_t host_completion_dependencies = 0;
    for (const auto & operations : collectives_)
        for (const auto & operation : operations) host_completion_dependencies += operation.completion_entry_dependencies.size();
    return {
        {                               "status",       gaps.at("total") == 0 ? "observed" : "partial"                                                 },
        {                                 "gaps",                                                                                       std::move(gaps) },
        {                  "dependency_coverage",                                                                                             "partial" },
        { "host_message_completion_dependencies",
         { { "basis", "native_protocol_rule" }, { "constraints", host_completion_dependencies }, { "independent_raw_confirmation", false } }           },
        {                         "cpu_services",                                                                                        std::move(cpu) },
        {                     "kv_page_identity",
         "physical page numbers are opaque; page aliases are not observed; logical demand and lifetime use native allocator lengths and release scopes" },
        {             "owned_hardware_intervals",                                                                                              hardware },
        {           "covered_hardware_intervals",                                                                                      covered_hardware },
        {                   "native_submissions",                                                                                           submissions },
        {           "covered_native_submissions",                                                                                   covered_submissions }
    };
}

} // namespace markov
