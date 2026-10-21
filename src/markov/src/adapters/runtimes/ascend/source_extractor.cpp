#include "markov/adapters/runtimes/ascend/source_extractor.hpp"

#include "markov/contracts/json_input.hpp"

#include "markov/production/block_binding.hpp"
#include "markov/production/block_extractor.hpp"
#include "markov/production/source_bindings.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace markov {

std::optional<DeviceCommandWork> AscendSourceExtractor::device_command(const nlohmann::json & interval) {
    if (interval.at("kind") != "device_marker" || interval.at("arguments").at("Task Type") != "EVENT_RECORD") return std::nullopt;

    // This is the physical record task. Its host submission and dependent wait
    // keep their own graph nodes and timing responsibilities.
    return DeviceCommandWork{ interval.at("arguments").at("Task Type").get<std::string>() };
}

namespace {

using input::read_json;

StepKind step_kind(std::string_view name) {
    // Native model_forward is an annotation within prefill/decode, not a sixth
    // serving step. Its phase is assigned from its actual parent below.
    if (name == "model_forward" || name == "model_region" || name == "component_region") return StepKind::Prefill;
    return ServingContract{}.block(name).kind;
}

SourceSpanKind span_kind(std::string_view name) {
    if (name == "cpu") return SourceSpanKind::CpuObservation;
    if (name == "compute") return SourceSpanKind::Compute;
    if (name == "dma") return SourceSpanKind::Dma;
    if (name == "device_wait") return SourceSpanKind::DeviceWait;
    if (name == "device_marker") return SourceSpanKind::DeviceMarker;
    if (name == "communication_detail") return SourceSpanKind::CommunicationObservation;
    throw std::invalid_argument("unknown native interval kind: " + std::string(name));
}

} // namespace

void AscendSourceExtractor::read_calls(const nlohmann::json & records) {
    for (const auto & [identity, record] : records.items()) {
        const std::string name = record.at("step").get<std::string>();
        SourceCall call{};
        call.identity = identity;
        call.step = step_kind(name);
        call.kind = name == "model_forward"                                  ? SourceCallKind::ForwardInputs
                    : (name == "model_region" || name == "component_region") ? SourceCallKind::NativeRegion
                                                                             : SourceCallKind::Serving;
        call.rank = record.at("rank").get<std::uint32_t>();
        call.observed_start_ns = record.at("start_ns").get<std::int64_t>();

        const auto & binding = record.at("binding");
        if (!binding.is_null()) call.binding = binding.get<std::string>();

        const auto & metadata = record.at("metadata");
        if (name == "schedule") {
            const auto & result = record.at("result");
            if (result.contains("input_supported")) {
                call.input_supported = result.at("input_supported").get<bool>();
                for (const auto & request : result.at("input_requests")) {
                    call.incoming_requests.push_back({ request.at("id").get<std::string>(),
                                                       request.at("prompt_tokens").get<std::uint32_t>(),
                                                       request.at("output_tokens").get<std::uint32_t>() });
                }
            }
            const auto policy = metadata.find("admission_policy");
            if (policy != metadata.end()) { call.admission_policy = SourceAdmissionPolicy::read(*policy); }
            for (const auto & filter : record.at("result").at("batch_filters")) {
                call.batch_filters.push_back({ filter.at("before").get<std::vector<std::string>>(), filter.at("after").get<std::vector<std::string>>() });
            }
        }
        if (name == "publish") {
            const auto policy = record.at("result").find("output_policy");
            if (policy != record.at("result").end()) {
                call.output_policy = SourceOutputPolicy{ input::positive(policy->at("force_interval_tokens")), policy->at("non_streaming").get<bool>() };
            }

            for (const auto & request : record.at("result").at("requests")) {
                call.publications.push_back({ request.at("id").get<std::string>(), request.at("finished").get<bool>() });
            }
        }

        if (name != "schedule" && call.kind != SourceCallKind::NativeRegion) {
            call.batch_id = metadata.at("batch_id").get<std::uint64_t>();
            const auto phase = metadata.at("phase").get<std::string>();
            if (phase != "prefill" && phase != "decode") throw std::invalid_argument("unknown source batch phase: " + phase);
            call.phase = phase == "prefill" ? BatchPhase::Prefill : BatchPhase::Decode;
            for (const auto & request : metadata.at("requests")) {
                call.requests.push_back({ request.at("id").get<std::string>(),
                                          request.at("input_tokens").get<std::uint32_t>(),
                                          request.at("context_tokens").get<std::uint32_t>(),
                                          request.at("prompt_tokens").get<std::uint32_t>(),
                                          request.at("generated_tokens").get<std::uint32_t>() });
            }
        }
        else if (name == "schedule" && !record.at("result").at("batch_id").is_null()) {
            call.batch_id = record.at("result").at("batch_id").get<std::uint64_t>();
        }

        call_ids_.emplace(identity, calls_.size());
        calls_.push_back(std::move(call));
    }

    // Resolve parent identities after every call has entered the lookup table.
    for (auto & call : calls_) {
        const auto & parent = records.at(call.identity).at("parent_id");
        if (!parent.is_null()) { call.parent = call_ids_.at(std::to_string(call.rank) + ":" + std::to_string(parent.get<std::uint64_t>())); }
        if (call.is_annotation() || call.step != StepKind::Schedule || !call.input_supported) continue;

        const auto & result = records.at(call.identity).at("result");
        const auto resolve = [&](const nlohmann::json & identity) {
            return call_ids_.at(std::to_string(call.rank) + ":" + std::to_string(identity.get<std::uint64_t>()));
        };
        if (!result.at("input_ready").is_null()) call.input_ready_call = resolve(result.at("input_ready"));
        for (const auto & request : result.at("request_ready")) {
            call.request_ready_calls.emplace_back(request.at("id").get<std::string>(), resolve(request.at("call_id")));
        }
    }

    // The JSON object is not in call-tree order. Resolve all parent links before
    // inheriting serving semantics through arbitrarily nested model regions.
    for (auto & call : calls_) {
        const auto parent = call.serving_parent(calls_);
        if (call.is_annotation()) { call.step = calls_[parent.value()].step; }
        if (!call.is_annotation() && call.step == StepKind::Prepare && parent) {
            auto & schedule = calls_[*parent];
            schedule.phase = call.phase;
            schedule.requests = call.requests;
        }
    }
}

void AscendSourceExtractor::read_intervals(const nlohmann::json & intervals) {
    std::size_t collective_roots = 0;
    std::size_t observer_markers = 0;
    std::map<std::pair<std::size_t, std::size_t>, std::vector<SourceSpanId>> operator_intervals;
    for (const auto & interval : intervals) {
        const std::size_t identity = interval.at("id").get<std::size_t>();
        const auto begin_ns = interval.at("start_ns").get<std::int64_t>();
        const auto end_ns = interval.at("end_ns").get<std::int64_t>();
        if (identity != graph_.spans().size() || end_ns < begin_ns) {
            throw std::invalid_argument("source interval identity/order or duration is invalid at " + std::to_string(identity));
        }

        const std::string name = interval.at("name").get<std::string>();
        auto kind = span_kind(interval.at("kind").get<std::string>());
        if (kind == SourceSpanKind::CpuObservation && (name == "MarkovWorkspaceAllocate" || name == "MarkovWorkspaceRelease"))
            kind = SourceSpanKind::ResourceObservation;
        SourceDomain domain = SourceDomain::DeviceStream;
        if (kind == SourceSpanKind::CpuObservation || kind == SourceSpanKind::ResourceObservation) domain = SourceDomain::CpuThread;
        if (kind == SourceSpanKind::CommunicationObservation) domain = SourceDomain::CommunicationObservation;

        const auto rank = interval.at("rank").get<std::uint32_t>();
        const auto native_id = interval.at("tid").get<std::uint64_t>();
        const auto lane = graph_.lane(domain, rank, native_id);
        std::optional<std::size_t> owner;
        if (!interval.at("owner_call").is_null()) { owner = call_ids_.at(interval.at("owner_call").get<std::string>()); }
        std::optional<std::size_t> scope;
        if (!interval.at("scope_call").is_null()) scope = call_ids_.at(interval.at("scope_call").get<std::string>());
        const auto span_id = graph_.observe(kind, lane, begin_ns, end_ns, owner, identity, std::nullopt, scope);

        if (kind == SourceSpanKind::CpuObservation && name == "markov_observer" && begin_ns < end_ns) {
            observer_windows_[lane].emplace_back(begin_ns, end_ns);
            ++observer_markers;
        }
        if (kind == SourceSpanKind::ResourceObservation) {
            // Allocation is a replaceable resource operation inside a worker.
            // Its marker must cut CPU service too, not only annotate the graph.
            retain_cpu_window(span_id);
        }
        if (kind == SourceSpanKind::CpuObservation && owner && scope && interval.at("operator_function").get<bool>())
            operator_intervals[{ lane, *scope }].push_back(span_id);
        if (const auto kernel = interval.find("kernel"); kernel != interval.end()) {
            graph_.observe_kernel(span_id,
                                  { name,
                                    kernel->at("Type").get<std::string>(),
                                    kernel->at("Input Shapes").get<std::string>(),
                                    kernel->at("Output Shapes").get<std::string>(),
                                    kernel->at("Input Data Types").get<std::string>(),
                                    kernel->at("Output Data Types").get<std::string>(),
                                    kernel->at("Input Formats").get<std::string>(),
                                    kernel->at("Output Formats").get<std::string>() });
        }

        if ((name.starts_with("markov/") || name.starts_with("markov_pending/")) && !interval.value("ignored", false)) {
            const auto separator = name.find('/', name.find('/') + 1);
            const auto call = call_ids_.at(std::to_string(rank) + ":" + name.substr(separator + 1));
            calls_[call].marker = span_id;
        }

        if (kind == SourceSpanKind::CommunicationObservation && name.starts_with("hcom_")) ++collective_roots;
    }

    // Profiler events need not follow their nesting order. Collect every marker
    // before classifying functions, then merge nested or touching observer work.
    std::size_t observer_ranges = 0;
    for (auto & [lane, windows] : observer_windows_) {
        std::ranges::sort(windows);
        std::vector<std::pair<std::int64_t, std::int64_t>> merged;
        for (const auto & window : windows) {
            if (merged.empty() || merged.back().second < window.first) merged.push_back(window);
            else merged.back().second = std::max(merged.back().second, window.second);
        }
        windows = std::move(merged);
        observer_ranges += windows.size();
    }

    // A backend launch may be nested in a function that also allocates
    // or copies storage. Delimit the outer operation, not each internal helper.
    std::size_t observer_functions = 0;
    for (auto & [location, members] : operator_intervals) {
        std::sort(members.begin(), members.end(), [&](SourceSpanId left, SourceSpanId right) {
            const auto & a = graph_.spans()[left];
            const auto & b = graph_.spans()[right];
            const auto a_begin = graph_.boundaries()[a.begin].observed_time_ns;
            const auto b_begin = graph_.boundaries()[b.begin].observed_time_ns;
            if (a_begin != b_begin) return a_begin < b_begin;
            return graph_.boundaries()[a.end].observed_time_ns > graph_.boundaries()[b.end].observed_time_ns;
        });
        std::optional<std::int64_t> outer_end;
        for (const auto identity : members) {
            if (observer_work(identity)) {
                ++observer_functions;
                continue;
            }
            const auto & span = graph_.spans()[identity];
            const auto begin = graph_.boundaries()[span.begin].observed_time_ns;
            const auto end = graph_.boundaries()[span.end].observed_time_ns;
            if (outer_end && begin < *outer_end) {
                if (end > *outer_end) throw std::invalid_argument("CPU operator observations overlap without nesting");
                continue;
            }
            cpu_operation_roots_[location].push_back(identity);
            outer_end = end;
        }
    }

    if (observer_markers != 0)
        gaps_.push_back({
            {      "kind",                      "scope" },
            {    "reason", "observer_cpu_work_excluded" },
            {   "markers",             observer_markers },
            {    "ranges",              observer_ranges },
            { "functions",           observer_functions }
        });

    if (collective_roots != 0)
        gaps_.push_back({
            {      "kind",                                       "cost" },
            {    "reason", "collective_transport_service_not_separated" },
            { "intervals",                             collective_roots }
        });
}

bool AscendSourceExtractor::observer_work(SourceSpanId identity) const {
    const auto & span = graph_.spans()[identity];
    // Queue/flow propagation identifies asynchronous auxiliary work even when
    // it runs outside the recording thread's synchronous observer window.
    if (span.source_interval && facts_.at("intervals").at(*span.source_interval).value("observer", false)) return true;
    if (span.kind != SourceSpanKind::CpuObservation && span.kind != SourceSpanKind::ResourceObservation) return false;
    const auto & begin = graph_.boundaries()[span.begin];
    const auto found = observer_windows_.find(begin.lane);
    if (found == observer_windows_.end()) return false;

    const auto & windows = found->second;
    const auto position = std::upper_bound(windows.begin(), windows.end(), begin.observed_time_ns, [](std::int64_t time_ns, const auto & window) {
        return time_ns < window.first;
    });
    return position != windows.begin() && graph_.boundaries()[span.end].observed_time_ns <= std::prev(position)->second;
}

SourceSpanId AscendSourceExtractor::operation_host(SourceSpanId identity) const {
    const auto & span = graph_.spans()[identity];
    const auto & begin = graph_.boundaries()[span.begin];
    const auto end = graph_.boundaries()[span.end].observed_time_ns;
    const auto roots = cpu_operation_roots_.find({ begin.lane, *span.scope_call });
    if (roots != cpu_operation_roots_.end()) {
        for (const auto root : roots->second) {
            const auto & enclosing = graph_.spans()[root];
            if (graph_.boundaries()[enclosing.begin].observed_time_ns <= begin.observed_time_ns && end <= graph_.boundaries()[enclosing.end].observed_time_ns)
                return root;
        }
    }
    return identity;
}

void AscendSourceExtractor::read_operations(const nlohmann::json & intervals, const nlohmann::json & flows) {
    std::map<SourceSpanId, SourceOperation> operations;
    const auto & collectives = graph_.collectives();
    // A view-only branch still performs CPU work. Operation identity must not
    // depend on whether this invocation happened to submit a device task.
    for (const auto & [location, roots] : cpu_operation_roots_) {
        for (const auto host : roots) operations.emplace(host, SourceOperation{ intervals[host].at("name").get<std::string>(), host, {}, {} });
    }

    for (const auto & flow : flows) {
        if (flow.at("category") != "async_npu" || flow.at("source_intervals").size() != 1) continue;
        const auto call = flow.at("source_intervals")[0].get<SourceSpanId>();
        const auto & span = graph_.spans()[call];
        if (span.kind != SourceSpanKind::CpuObservation || !span.owner_call || !span.scope_call || observer_work(call)) continue;
        const auto host = operation_host(call);
        for (const auto & target : flow.at("target_intervals")) {
            const auto device = target.get<SourceSpanId>();
            const auto & work = graph_.spans()[device];
            if (work.owner_call != span.owner_call || work.scope_call != span.scope_call) continue;
            auto [position, inserted] = operations.try_emplace(host, SourceOperation{ intervals[host].at("name").get<std::string>(), host, {}, {} });
            const auto collective = collectives.find(device);
            if (collective == collectives.end()) {
                position->second.device_spans.push_back(device);
                continue;
            }

            // The async flow names an envelope, but the operation owns its
            // associated physical tasks. Keep the envelope only as provenance.
            position->second.collective_spans.push_back(device);
            for (const auto & task : collective->second.tasks) position->second.device_spans.push_back(task.span);
        }
    }

    std::map<std::pair<std::size_t, std::size_t>, std::vector<SourceSpanId>> hosts;
    for (const auto & [identity, operation] : operations) {
        const auto & span = graph_.spans()[identity];
        hosts[{ graph_.boundaries()[span.begin].lane, *span.scope_call }].push_back(identity);
    }
    for (const auto & flow : flows) {
        if (flow.at("category") != "async_task_queue" || flow.at("source_intervals").size() != 1 || flow.at("target_intervals").size() != 1) continue;
        const auto enqueue = flow.at("source_intervals")[0].get<SourceSpanId>();
        const auto dequeue = flow.at("target_intervals")[0].get<SourceSpanId>();
        const auto & source = graph_.spans()[enqueue];
        const auto & worker = graph_.spans()[dequeue];
        if (!source.scope_call || source.scope_call != worker.scope_call || source.owner_call != worker.owner_call || observer_work(enqueue)) continue;
        const auto & begin = graph_.boundaries()[source.begin];
        const auto end = graph_.boundaries()[source.end].observed_time_ns;
        const auto candidates = hosts.find({ begin.lane, *source.scope_call });
        if (candidates == hosts.end()) continue;

        // Queue identity establishes the asynchronous association. Containment
        // only checks the enqueue's unique synchronous host operation.
        std::vector<SourceSpanId> enclosing;
        for (const auto host : candidates->second) {
            const auto & span = graph_.spans()[host];
            if (graph_.boundaries()[span.begin].observed_time_ns <= begin.observed_time_ns && end <= graph_.boundaries()[span.end].observed_time_ns)
                enclosing.push_back(host);
        }
        if (enclosing.size() == 1) operations.at(enclosing.front()).worker_spans.push_back(dequeue);
    }

    for (auto & [identity, operation] : operations) {
        // Keep the CPU profiler's operand record, including scalar-list slots.
        // Missing metadata stays absent; it is not an empty tensor signature.
        const auto & arguments = intervals[identity].at("arguments");
        if (arguments.contains("Input Dims")) operation.input_shapes = arguments.at("Input Dims").get<std::string>();
        if (arguments.contains("Input type")) operation.input_types = arguments.at("Input type").get<std::string>();

        std::sort(operation.collective_spans.begin(), operation.collective_spans.end());
        operation.collective_spans.erase(std::unique(operation.collective_spans.begin(), operation.collective_spans.end()), operation.collective_spans.end());
        std::sort(operation.device_spans.begin(), operation.device_spans.end());
        operation.device_spans.erase(std::unique(operation.device_spans.begin(), operation.device_spans.end()), operation.device_spans.end());
        std::sort(operation.worker_spans.begin(), operation.worker_spans.end());
        operation.worker_spans.erase(std::unique(operation.worker_spans.begin(), operation.worker_spans.end()), operation.worker_spans.end());

        graph_.observe_operation(std::move(operation));
    }
}

void AscendSourceExtractor::read_cpu_scopes() {
    struct Endpoint {
        std::int64_t time_ns;
        std::size_t call;
        std::size_t depth;
        bool entering;
    };
    std::map<std::size_t, std::vector<Endpoint>> endpoints;
    for (std::size_t index = 0; index < calls_.size(); ++index) {
        const auto & call = calls_[index];
        if (!call.is_annotation() || !call.marker) continue;
        const auto & marker = graph_.spans()[*call.marker];
        const auto & begin = graph_.boundaries()[marker.begin];
        const auto & end = graph_.boundaries()[marker.end];
        if (begin.observed_time_ns == end.observed_time_ns) continue;

        std::size_t depth = 0;
        for (auto parent = call.parent; parent; parent = calls_[*parent].parent) ++depth;
        endpoints[begin.lane].push_back({ begin.observed_time_ns, index, depth, true });
        endpoints[begin.lane].push_back({ end.observed_time_ns, index, depth, false });
    }

    // These are explicit synchronous wrapper identities on their recorded lane,
    // not temporal matching of asynchronous work to a nearby model invocation.
    for (auto & [lane, points] : endpoints) {
        std::sort(points.begin(), points.end(), [](const Endpoint & left, const Endpoint & right) {
            if (left.time_ns != right.time_ns) return left.time_ns < right.time_ns;
            if (left.entering != right.entering) return !left.entering;
            return left.entering ? left.depth < right.depth : left.depth > right.depth;
        });
        std::vector<std::size_t> active;
        for (const auto & point : points) {
            if (point.entering) {
                if (!active.empty() && calls_[point.call].parent != active.back())
                    throw std::invalid_argument("native CPU region markers disagree with the recorded parent stack");
                active.push_back(point.call);
            }
            else {
                if (active.empty() || active.back() != point.call) throw std::invalid_argument("native CPU region markers overlap without nesting");
                active.pop_back();
            }
            cpu_scope_changes_[lane][point.time_ns] = active.empty() ? std::nullopt : std::optional(active.back());
            cpu_service_boundaries_[lane].insert(point.time_ns);
        }
    }
}

void AscendSourceExtractor::read_flows(const nlohmann::json & flows) {
    std::size_t index = 0;
    for (const auto & flow : flows) {
        bool observer_submission = false;
        // The vendor flow is anchored at API/queue start, not at API return.
        // Connecting return to device start would manufacture a CPU/NPU barrier.
        for (const auto & source : flow.at("source_intervals")) {
            const auto source_id = source.get<SourceSpanId>();
            for (const auto & target : flow.at("target_intervals")) {
                const auto target_id = target.get<SourceSpanId>();
                const bool auxiliary_source = observer_work(source_id), auxiliary_target = observer_work(target_id);
                // The raw queue identities still document auxiliary work. Its
                // internal flows must not create business CPU cuts or imports.
                if (auxiliary_source && auxiliary_target) continue;
                graph_.connect(graph_.spans().at(source_id).begin, graph_.spans().at(target_id).begin, SourceRelation::NativeFlow, index);
                const auto & target_span = graph_.spans()[target_id];
                if (auxiliary_source && !auxiliary_target && (flow.at("category") == "async_task_queue" || target_span.kind != SourceSpanKind::CpuObservation))
                    observer_submission = true;
            }
        }
        if (observer_submission)
            gaps_.push_back({
                {             "kind",                               "observation" },
                {           "reason", "observer_scope_contains_native_submission" },
                {             "flow",                                       index },
                { "source_intervals",                 flow.at("source_intervals") },
                { "target_intervals",                 flow.at("target_intervals") }
            });
        ++index;
    }
}

bool AscendSourceExtractor::read_task_dependencies(const std::vector<SourceSpanId> & spans, const nlohmann::json & intervals, const nlohmann::json & record,
                                                   std::size_t source_api) {
    const auto & description = record.at("ffts");
    if (!description.at("gap").is_null()) return false;

    const auto & contexts = description.at("contexts");
    if (contexts.size() != description.at("context_count")) return false;

    // Native context identities, not timestamps or numeric order, select the
    // endpoints. Unconditional LABEL contexts carry ordering but no observed
    // physical service interval; conditional/loop contexts remain unsupported.
    std::map<std::size_t, SourceSpanId> physical;
    for (const auto span : spans) {
        const auto context = intervals.at(span).at("arguments").at("Subtask Id").get<std::size_t>();
        if (!physical.emplace(context, span).second) return false;
    }

    std::vector<std::size_t> predecessors(contexts.size());
    std::size_t hardware_contexts = 0;
    for (std::size_t index = 0; index < contexts.size(); ++index) {
        const auto & context = contexts[index];
        if (context.at("id") != index || context.at("auto_thread") != false || context.at("thread_id") != 0 || context.at("thread_dim") != 1) return false;

        const auto type = context.at("type").get<std::uint16_t>();
        constexpr std::uint16_t label_type = 0x0500; // CANN RT_CTX_TYPE_LABEL.
        if (type == label_type) {
            if (physical.contains(index)) return false;
        }
        else {
            if (!physical.contains(index)) return false;
            ++hardware_contexts;

            const auto & name = intervals.at(physical.at(index)).at("name");
            // CANN rtFftsPlusContextType_t hardware values, not cost constants.
            if (!((type == 8 && name == "SDMA") || (type == 3 && name == "Notify Wait") || (type == 4 && name == "Notify Record")
                  || (type == 5 && name == "Write Value")))
                return false;
        }

        for (const auto & value : context.at("successors")) {
            const auto successor = value.get<std::size_t>();
            if (successor >= contexts.size() || successor == index) return false;
            ++predecessors[successor];
        }
    }
    if (hardware_contexts != physical.size()) return false;

    std::vector<std::size_t> ready;
    for (std::size_t index = 0; index < contexts.size(); ++index) {
        if (contexts[index].at("predecessors") != predecessors[index]) return false;
        if (predecessors[index] == 0) ready.push_back(index);
    }
    if (ready.size() != description.at("ready_count")) return false;

    // Propagate the nearest physical predecessors across LABEL-only paths.
    // A fork keeps both successors; a join retains every required predecessor.
    std::vector<std::set<SourceSpanId>> incoming(contexts.size());
    std::set<std::pair<SourceSpanId, SourceSpanId>> dependencies;
    for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
        const auto index = ready[cursor];
        const auto endpoint = physical.find(index);
        if (endpoint != physical.end()) {
            for (const auto predecessor : incoming[index]) dependencies.emplace(predecessor, endpoint->second);
            incoming[index] = { endpoint->second };
        }

        for (const auto & value : contexts[index].at("successors")) {
            const auto successor = value.get<std::size_t>();
            incoming[successor].insert(incoming[index].begin(), incoming[index].end());
            if (--predecessors[successor] == 0) ready.push_back(successor);
        }
    }
    if (ready.size() != contexts.size()) return false;

    // Commit only after the full descriptor DAG is checked, including labels
    // absent from the hardware timeline. Retain the native descriptor source.
    for (const auto & [source, target] : dependencies) {
        graph_.connect(graph_.spans()[source].end, graph_.spans()[target].begin, SourceRelation::NativeTaskDependency, source_api);
    }
    return true;
}

void AscendSourceExtractor::read_device_order(const nlohmann::json & intervals, const nlohmann::json & records) {
    struct Task {
        std::int64_t begin_ns;
        std::int64_t end_ns;
        std::vector<SourceSpanId> spans;
    };
    using StreamKey = std::pair<std::uint32_t, std::uint64_t>;
    using TaskKey = std::tuple<std::uint32_t, std::uint64_t, std::uint64_t>;
    std::map<TaskKey, std::vector<std::size_t>> submissions;
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto & record = records[index];
        if (record.at("api") != "rtGeneralCtrl" || record.at("result") != 0 || record.at("hardware_task_id").is_null()
            || record.at("submitted_stream_id") != record.at("stream_id"))
            continue;

        submissions[{ record.at("rank").get<std::uint32_t>(),
                      record.at("submitted_stream_id").get<std::uint64_t>(),
                      record.at("hardware_task_id").get<std::uint64_t>() }]
            .push_back(index);
    }

    std::map<StreamKey, std::map<std::uint64_t, Task>> streams;
    for (const auto & interval : intervals) {
        if (interval.at("process") != "Ascend Hardware") continue;

        const auto & args = interval.at("arguments");

        const auto rank = interval.at("rank").get<std::uint32_t>();
        const auto stream = args.at("Physic Stream Id").get<std::uint64_t>();
        const auto task_id = args.at("Task Id").get<std::uint64_t>();
        const auto begin_ns = interval.at("start_ns").get<std::int64_t>();
        const auto end_ns = interval.at("end_ns").get<std::int64_t>();

        const auto position = streams[StreamKey(rank, stream)].try_emplace(task_id, Task{ begin_ns, end_ns, {} }).first;
        auto & task = position->second;
        task.begin_ns = std::min(task.begin_ns, begin_ns);
        task.end_ns = std::max(task.end_ns, end_ns);
        task.spans.push_back(interval.at("id").get<SourceSpanId>());
    }

    std::size_t compound_tasks = 0;
    std::size_t next_chain = 0;
    for (auto & [stream, tasks] : streams) {
        std::vector<const Task *> sequence;
        for (const auto & [identity, task] : tasks) {
            sequence.push_back(&task);
            if (task.spans.size() > 1) {
                const auto & matches = submissions[{ stream.first, stream.second, identity }];
                if (matches.size() != 1 || !read_task_dependencies(task.spans, intervals, records[matches.front()], matches.front())) ++compound_tasks;
            }
        }

        std::sort(sequence.begin(), sequence.end(), [](const Task * left, const Task * right) { return left->begin_ns < right->begin_ns; });

        auto chain = next_chain++;
        std::size_t position = 0;
        std::optional<BoundaryId> preceding_done;
        std::optional<std::int64_t> preceding_end_ns;
        for (const Task * task : sequence) {
            if (preceding_done && task->begin_ns >= *preceding_end_ns) {
                for (const auto span : task->spans) graph_.connect(*preceding_done, graph_.spans()[span].begin, SourceRelation::StreamOrder);
            }
            else if (preceding_done) {
                chain = next_chain++;
                position = 0;
                gaps_.push_back({
                    {        "kind",                       "observation" },
                    {      "reason", "device_stream_task_groups_overlap" },
                    {        "rank",                        stream.first },
                    {      "stream",                       stream.second },
                    { "source_span",                 task->spans.front() }
                });
            }

            for (const auto span : task->spans) device_order_.emplace(span, std::pair(chain, position));
            ++position;
            preceding_done = graph_.complete(task->spans, stream.first, task->spans.front(), SourceDomain::DeviceTaskCompletion);
            preceding_end_ns = task->end_ns;
        }
    }

    if (compound_tasks != 0) {
        gaps_.push_back({
            {   "kind",                                        "observation" },
            { "reason", "compound_device_task_internal_dependencies_partial" },
            {  "tasks",                                       compound_tasks }
        });
    }
}

bool AscendSourceExtractor::device_precedes(SourceSpanId before, SourceSpanId after,
                                            const std::map<BoundaryId, std::vector<BoundaryId>> & task_predecessors) const {
    const auto left = device_order_.at(before);
    const auto right = device_order_.at(after);
    if (left.first != right.first) return false;
    if (left.second != right.second) return left.second < right.second;

    // Within one compound task, only descriptor dependencies order contexts.
    const auto expected = graph_.spans()[before].end;
    std::vector<BoundaryId> pending{ graph_.spans()[after].begin };
    std::set<BoundaryId> visited;
    while (!pending.empty()) {
        const auto point = pending.back();
        pending.pop_back();
        if (point == expected) return true;
        if (!visited.insert(point).second) continue;

        const auto predecessors = task_predecessors.find(point);
        if (predecessors != task_predecessors.end()) pending.insert(pending.end(), predecessors->second.begin(), predecessors->second.end());
    }
    return false;
}

bool AscendSourceExtractor::order_notifications(std::vector<Notification> & notifications,
                                                const std::map<BoundaryId, std::vector<BoundaryId>> & task_predecessors) const {
    std::vector<std::size_t> predecessors(notifications.size());
    for (std::size_t left = 0; left < notifications.size(); ++left) {
        for (std::size_t right = left + 1; right < notifications.size(); ++right) {
            const bool forward = device_precedes(notifications[left].span, notifications[right].span, task_predecessors);
            const bool backward = device_precedes(notifications[right].span, notifications[left].span, task_predecessors);
            if (forward == backward) return false;

            ++predecessors[forward ? right : left];
        }
    }

    std::vector<Notification> ordered(notifications.size());
    for (std::size_t index = 0; index < notifications.size(); ++index) ordered[predecessors[index]] = notifications[index];
    notifications = std::move(ordered);
    return true;
}

void AscendSourceExtractor::read_communication(const nlohmann::json & intervals) {
    using TaskKey = std::tuple<std::uint32_t, std::uint64_t, std::uint64_t, std::uint64_t>;
    using NotifyKey = std::tuple<std::string, std::string, std::uint32_t, std::uint32_t>;
    struct Generations {
        std::vector<Notification> records;
        std::vector<Notification> waits;
    };
    std::map<std::uint32_t, std::uint32_t> physical_ranks;
    for (const auto & device : facts_.at("devices")) {
        const auto physical = device.at("physical_rank").get<std::uint32_t>();
        const auto [entry, inserted] = physical_ranks.emplace(device.at("rank").get<std::uint32_t>(), physical);
        if (!inserted && entry->second != physical) throw std::invalid_argument("one source role has conflicting native process-group ranks");
    }
    std::map<TaskKey, std::vector<SourceSpanId>> tasks;
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::vector<std::size_t>> collectives;
    std::vector<SourceCollective> observations;
    for (const auto & interval : intervals) {
        const auto & args = interval.at("arguments");

        const auto rank = interval.at("rank").get<std::uint32_t>();
        if (interval.at("process") == "Ascend Hardware") {
            const TaskKey key(rank,
                              args.at("Physic Stream Id").get<std::uint64_t>(),
                              args.at("Task Id").get<std::uint64_t>(),
                              args.at("Subtask Id").get<std::uint64_t>());
            tasks[key].push_back(interval.at("id").get<SourceSpanId>());
        }
        if (interval.at("process") == "Communication" && interval.at("name").get<std::string>().starts_with("hcom_")) {
            const auto connection = args.at("connection_id").get<std::uint64_t>();
            collectives[{ rank, connection }].push_back(observations.size());
            observations.push_back({ interval.at("id").get<SourceSpanId>(),
                                     rank,
                                     connection,
                                     interval.at("name").get<std::string>(),
                                     args.at("count").get<std::uint64_t>(),
                                     args.at("data_type").get<std::string>(),
                                     args.at("alg_type").get<std::string>(),
                                     args.at("group_name").get<std::string>(),
                                     interval.at("communicator"),
                                     {} });
        }
    }

    std::map<NotifyKey, Generations> notifications;
    for (const auto & interval : intervals) {
        const std::string name = interval.at("name").get<std::string>();
        if (interval.at("process") != "Communication" || !interval.at("arguments").contains("context id")) continue;

        const auto rank = interval.at("rank").get<std::uint32_t>();
        const auto & args = interval.at("arguments");
        const auto stream = args.at("stream id").get<std::uint64_t>();
        const auto task = args.at("task id").get<std::uint64_t>();
        const auto subtask = args.at("context id").get<std::uint64_t>();
        const auto & physical = tasks[TaskKey(rank, stream, task, subtask)];
        const auto detail = interval.at("id").get<std::size_t>();
        if (physical.size() != 1 || args.at("src rank").get<std::uint32_t>() != physical_ranks.at(rank)) {
            gaps_.push_back({
                {     "kind",                                   "observation" },
                {   "reason", "communication_task_or_rank_identity_ambiguous" },
                { "interval",                                          detail }
            });
            continue;
        }

        const auto & hardware = intervals.at(physical.front());
        const auto collective = collectives.find(std::pair(rank, hardware.at("arguments").at("connection_id").get<std::uint64_t>()));
        if (collective == collectives.end() || collective->second.size() != 1) {
            gaps_.push_back({
                {     "kind",                                            "observation" },
                {   "reason", "communication_collective_identity_missing_or_ambiguous" },
                { "interval",                                                   detail }
            });
            continue;
        }

        // Data work and notifications share the same exact task association.
        // The profiler's physical dtype remains distinct from collective dtype.
        auto & observation = observations.at(collective->second.front());
        observation.tasks.push_back({ physical.front(),
                                      detail,
                                      stream,
                                      task,
                                      subtask,
                                      name,
                                      args.at("size(Byte)").get<std::uint64_t>(),
                                      args.at("data type").get<std::string>(),
                                      args.at("transport type").get<std::string>(),
                                      args.at("src rank").get<std::uint32_t>(),
                                      args.at("dst rank").get<std::uint32_t>() });
        if (name != "Notify_Record" && name != "Notify_Wait") continue;

        const auto peer = args.at("dst rank").get<std::uint32_t>();
        const auto producer = name == "Notify_Record" ? physical_ranks.at(rank) : peer;
        const auto consumer = name == "Notify_Record" ? peer : physical_ranks.at(rank);
        const NotifyKey key(observation.name, args.at("notify_id").get<std::string>(), producer, consumer);
        auto & generations = notifications[key];
        auto & sequence = name == "Notify_Record" ? generations.records : generations.waits;
        sequence.push_back({ physical.front(), detail });
    }

    std::map<BoundaryId, std::vector<BoundaryId>> task_predecessors;
    for (const auto & edge : graph_.edges())
        if (edge.relation == SourceRelation::NativeTaskDependency) task_predecessors[edge.target].push_back(edge.source);

    for (auto & observation : observations) {
        std::set<BoundaryId> member_ends;
        for (const auto & task : observation.tasks) member_ends.insert(graph_.spans()[task.span].end);

        // Host flows terminate at the collective annotation. Release physical
        // entry tasks from that same submission, not from envelope completion.
        for (const auto & task : observation.tasks) {
            const auto begin = graph_.spans()[task.span].begin;
            const auto & predecessors = task_predecessors[begin];
            const bool internal_predecessor = std::ranges::any_of(predecessors, [&](BoundaryId end) { return member_ends.contains(end); });
            if (!internal_predecessor) graph_.connect(graph_.spans()[observation.span].begin, begin, SourceRelation::CollectiveSubmission, observation.span);
        }
        graph_.observe_collective(std::move(observation));
    }

    // Traversal between descriptor edges also crosses each physical task extent.
    for (const auto & [identity, members] : tasks) {
        for (const auto member : members) {
            const auto & span = graph_.spans()[member];
            if (span.begin != span.end) task_predecessors[span.end].push_back(span.begin);
        }
    }

    for (auto & [key, generations] : notifications) {
        // Pair generations only after both local device paths establish a
        // total order. Task/context identifiers carry identity, not precedence.
        if (generations.records.size() != generations.waits.size() || !order_notifications(generations.records, task_predecessors)
            || !order_notifications(generations.waits, task_predecessors)) {
            gaps_.push_back({
                {          "kind",                                    "observation" },
                {        "reason", "native_notify_generations_not_uniquely_matched" },
                {    "collective",                                 std::get<0>(key) },
                {     "notify_id",                                 std::get<1>(key) },
                { "producer_rank",                                 std::get<2>(key) },
                { "consumer_rank",                                 std::get<3>(key) }
            });
            continue;
        }

        for (std::size_t index = 0; index < generations.records.size(); ++index) {
            const auto & record = generations.records[index];
            const auto & wait = generations.waits[index];
            graph_.connect(graph_.spans()[record.span].end, graph_.spans()[wait.span].end, SourceRelation::NativeNotifySignal, record.detail);
        }
    }
}

void AscendSourceExtractor::read_copy_boundaries(const nlohmann::json & records, const nlohmann::json & intervals) {
    auto unresolved_prefixes = nlohmann::json::array();
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto & record = records[index];
        if (record.value("ignored", false) || record.at("copy_execution") != "synchronous_driver") continue;
        const auto & interval = record.at("copy_interval");
        const auto begin_ns = interval.at("begin_ns").get<std::int64_t>();
        const auto end_ns = interval.at("end_ns").get<std::int64_t>();
        if (record.at("source_interval").is_null()) {
            gaps_.push_back({
                {       "kind",                       "observation" },
                {     "reason", "driver_copy_cpu_parent_unresolved" },
                { "source_api",                               index }
            });
            continue;
        }

        if (observer_work(record.at("source_interval").get<SourceSpanId>()))
            gaps_.push_back({
                {       "kind",                              "observation" },
                {     "reason", "observer_scope_contains_synchronous_copy" },
                { "source_api",                                      index }
            });

        const auto & parent = intervals.at(record.at("source_interval").get<std::size_t>());
        if (begin_ns < parent.at("start_ns").get<std::int64_t>() || end_ns > parent.at("end_ns").get<std::int64_t>()) {
            gaps_.push_back({
                {       "kind",                        "observation" },
                {     "reason", "driver_copy_parent_clock_disagrees" },
                { "source_api",                                index }
            });
            continue;
        }
        const auto lane = graph_.lane(SourceDomain::CpuThread, record.at("rank").get<std::uint32_t>(), record.at("tid").get<std::uint64_t>());
        cpu_service_boundaries_[lane].insert(begin_ns);
        cpu_service_boundaries_[lane].insert(end_ns);

        // Driver cuts retain CPU work on both sides of an observed internal
        // wait. Without that wait, the prefix still lacks execution semantics.
        const bool observed_wait = std::ranges::any_of(host_waits_, [this, index](auto identity) { return graph_.spans()[identity].source_api == index; });
        if (record.at("transfer_kind") == 1 && !observed_wait) unresolved_prefixes.push_back(index);
    }
    if (!unresolved_prefixes.empty()) {
        gaps_.push_back({
            {        "kind",                                         "observation" },
            {      "reason", "synchronous_copy_preparation_and_wait_not_separated" },
            { "source_apis",                        std::move(unresolved_prefixes) }
        });
    }
}

void AscendSourceExtractor::read_host_waits(const nlohmann::json & records, const nlohmann::json & intervals) {
    using Identity = std::pair<std::uint32_t, std::uint64_t>;
    const std::map<std::string, std::string> names{
        {            "rtStreamSynchronize",            "AscendCL@aclrtSynchronizeStream" },
        { "rtStreamSynchronizeWithTimeout", "AscendCL@aclrtSynchronizeStreamWithTimeout" },
        {            "rtDeviceSynchronize",                  "Runtime@DeviceSynchronize" },
        { "rtDeviceSynchronizeWithTimeout",                  "Runtime@DeviceSynchronize" }
    };
    std::map<Identity, std::vector<std::size_t>> cpu_connections;
    std::map<Identity, std::vector<std::size_t>> profiler_syncs;
    std::map<Identity, std::vector<std::size_t>> native_syncs;
    for (const auto & interval : intervals) {
        if (interval.at("process") != "CANN" || interval.contains("profiler_control") || interval.value("ignored", false)) continue;
        const auto rank = interval.at("rank").get<std::uint32_t>();
        const auto identity = interval.at("id").get<std::size_t>();
        cpu_connections[{ rank, interval.at("arguments").at("connection_id").get<std::uint64_t>() }].push_back(identity);
        const auto name = interval.at("name").get<std::string>();
        if (name == "AscendCL@aclrtSynchronizeStream" || name == "AscendCL@aclrtSynchronizeStreamWithTimeout" || name == "Runtime@DeviceSynchronize") {
            profiler_syncs[{ rank, interval.at("tid").get<std::uint64_t>() }].push_back(identity);
        }
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto & record = records[index];
        if (!record.contains("profiler_control") && !record.value("ignored", false) && names.contains(record.at("api").get<std::string>())) {
            native_syncs[{ record.at("rank").get<std::uint32_t>(), record.at("tid").get<std::uint64_t>() }].push_back(index);
        }
    }

    // Match complete sequences per thread and API kind. An unobserved device
    // synchronization must not invalidate independently observed stream waits.
    // Clock proximity is neither an identity rule nor a calibration offset.
    std::map<std::size_t, std::size_t> synchronized_calls;
    for (const auto & [thread, sequence] : native_syncs) {
        std::map<std::string, std::vector<std::size_t>> native_by_name;
        std::map<std::string, std::vector<std::size_t>> observed_by_name;
        for (const auto index : sequence) native_by_name[names.at(records[index].at("api").get<std::string>())].push_back(index);
        for (const auto index : profiler_syncs[thread]) observed_by_name[intervals[index].at("name").get<std::string>()].push_back(index);

        for (auto & [name, native] : native_by_name) {
            auto & observed = observed_by_name[name];
            std::sort(native.begin(), native.end(), [&records](auto left, auto right) { return records[left].at("start_ns") < records[right].at("start_ns"); });
            std::sort(observed.begin(), observed.end(), [&intervals](auto left, auto right) {
                return intervals[left].at("start_ns") < intervals[right].at("start_ns");
            });
            bool matched = native.size() == observed.size();
            for (std::size_t slot = 0; matched && slot < native.size(); ++slot)
                matched = records[native[slot]].at("owner_call") == intervals[observed[slot]].at("owner_call");

            if (!matched) {
                gaps_.push_back({
                    {           "kind",                          "observation" },
                    {         "reason", "native_host_sync_identity_unresolved" },
                    {           "rank",                           thread.first },
                    {         "thread",                          thread.second },
                    {       "api_kind",                                   name },
                    {   "native_calls",                          native.size() },
                    { "profiler_calls",                        observed.size() }
                });
                continue;
            }
            for (std::size_t slot = 0; slot < native.size(); ++slot) synchronized_calls.emplace(native[slot], observed[slot]);
        }
    }

    // Level2 exposes the synchronization inside a pageable H2D call. Its
    // containing intercepted API supplies stream identity; timestamps only
    // establish nesting, never a nearest-event match.
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto & record = records[index];
        if (record.contains("profiler_control") || record.value("ignored", false) || record.at("copy_execution") != "synchronous_driver"
            || record.at("transfer_kind") != 1 || record.at("source_interval").is_null())
            continue;

        std::vector<std::size_t> candidates;
        const auto begin = record.at("trace_interval").at("begin_ns").get<std::int64_t>();
        const auto end = record.at("copy_interval").at("begin_ns").get<std::int64_t>();
        for (const auto & interval : intervals) {
            if (interval.at("process") != "CANN" || interval.at("name") != "Runtime@StreamSynchronize" || interval.at("rank") != record.at("rank")
                || interval.at("tid") != record.at("tid"))
                continue;
            if (interval.at("start_ns").get<std::int64_t>() >= begin && interval.at("end_ns").get<std::int64_t>() <= end)
                candidates.push_back(interval.at("id").get<std::size_t>());
        }
        if (candidates.size() != 1) continue;

        synchronized_calls.emplace(index, candidates.front());
        native_syncs[{ record.at("rank").get<std::uint32_t>(), record.at("tid").get<std::uint64_t>() }].push_back(index);
    }

    std::set<std::size_t> associated_syncs;
    for (const auto & [api, interval] : synchronized_calls) associated_syncs.insert(interval);
    // Explicit ACL synchronization also contains a Runtime observation at
    // Level2. It describes the same wait, not another service or wait charge.
    for (const auto & interval : intervals) {
        if (interval.at("process") != "CANN" || interval.at("name") != "Runtime@StreamSynchronize") continue;
        for (const auto & [api, parent_id] : synchronized_calls) {
            const auto & parent = intervals[parent_id];
            if (parent.at("rank") == interval.at("rank") && parent.at("tid") == interval.at("tid") && parent.at("start_ns") <= interval.at("start_ns")
                && interval.at("end_ns") <= parent.at("end_ns")) {
                associated_syncs.insert(interval.at("id").get<std::size_t>());
                break;
            }
        }
    }
    auto profiler_controls = nlohmann::json::array();
    auto unassociated = nlohmann::json::array();
    for (const auto & interval : intervals) {
        if (interval.at("kind") == "cpu" && interval.at("name").get<std::string>().find("Synchronize") != std::string::npos
            && !associated_syncs.contains(interval.at("id").get<std::size_t>())) {
            if (interval.contains("profiler_control")) profiler_controls.push_back(interval.at("id"));
            else unassociated.push_back(interval.at("id"));
        }
    }
    if (!profiler_controls.empty()) {
        gaps_.push_back({
            {             "kind",                                            "scope" },
            {           "reason", "profiler_control_synchronizations_outside_target" },
            { "source_intervals",                       std::move(profiler_controls) }
        });
    }
    if (!unassociated.empty()) {
        gaps_.push_back({
            {             "kind",                           "observation" },
            {           "reason", "host_synchronization_scope_unresolved" },
            { "source_intervals",                            unassociated }
        });
    }

    struct Submission {
        SourceSpanId hardware;
        std::size_t cpu;
        std::int64_t begin_ns;
        std::int64_t end_ns;
    };
    std::map<Identity, std::vector<Submission>> submissions;
    std::map<Identity, std::set<std::size_t>> stream_lanes;
    std::map<std::size_t, std::set<Identity>> lane_streams;
    std::map<Identity, std::size_t> unmapped;
    auto observation_tasks = nlohmann::json::array();
    for (const auto & interval : intervals) {
        if (interval.at("process") != "Ascend Hardware") continue;
        const auto rank = interval.at("rank").get<std::uint32_t>();
        const auto & args = interval.at("arguments");
        // Profiler control stays in the source graph, outside target application work.
        // Unknown tasks remain unresolved; owner absence is not an exclusion rule.
        const auto & task = args.at("Task Type");
        if (interval.at("kind") == "device_marker" && (task == "PROFILING_ENABLE" || task == "PROFILING_DISABLE")) {
            observation_tasks.push_back(interval.at("id"));
            continue;
        }

        const Identity stream(rank, args.at("Physic Stream Id").get<std::uint64_t>());
        const auto lane = graph_.lane(SourceDomain::DeviceStream, rank, interval.at("tid").get<std::uint64_t>());
        stream_lanes[stream].insert(lane);
        lane_streams[lane].insert(stream);
        const auto & sources = cpu_connections[{ rank, args.at("connection_id").get<std::uint64_t>() }];
        if (sources.size() != 1) {
            ++unmapped[stream];
            continue;
        }
        const auto & source = intervals[sources.front()];
        submissions[stream].push_back(
            { interval.at("id").get<SourceSpanId>(), sources.front(), source.at("start_ns").get<std::int64_t>(), source.at("end_ns").get<std::int64_t>() });
    }

    if (!observation_tasks.empty()) {
        gaps_.push_back({
            {             "kind",                                                 "scope" },
            {           "reason", "profiler_control_tasks_outside_target_synchronization" },
            { "source_intervals",                            std::move(observation_tasks) }
        });
    }

    for (const auto & [thread, sequence] : native_syncs) {
        for (const auto index : sequence) {
            const auto & record = records[index];
            const auto observed = synchronized_calls.find(index);
            if (observed != synchronized_calls.end() && observer_work(observed->second))
                gaps_.push_back({
                    {       "kind",                         "observation" },
                    {     "reason", "observer_scope_contains_native_wait" },
                    { "source_api",                                 index }
                });

            const bool internal_copy_wait = record.at("copy_execution") == "synchronous_driver";
            const auto begin_ns = internal_copy_wait ? intervals[synchronized_calls.at(index)].at("start_ns").get<std::int64_t>()
                                                     : record.at("trace_interval").at("begin_ns").get<std::int64_t>();
            const auto end_ns = internal_copy_wait ? intervals[synchronized_calls.at(index)].at("end_ns").get<std::int64_t>()
                                                   : record.at("trace_interval").at("end_ns").get<std::int64_t>();
            const auto lane = graph_.lane(SourceDomain::CpuThread, thread.first, thread.second);
            std::optional<std::size_t> owner;
            if (!record.at("owner_call").is_null()) owner = call_ids_.at(record.at("owner_call").get<std::string>());
            std::optional<std::size_t> scope;
            if (!record.at("scope_call").is_null()) scope = call_ids_.at(record.at("scope_call").get<std::string>());
            const auto source_interval = observed == synchronized_calls.end() ? std::nullopt : std::optional(observed->second);
            const auto wait = graph_.observe(SourceSpanKind::HostWait, lane, begin_ns, end_ns, owner, source_interval, index, scope);
            host_waits_.push_back(wait);

            // The synchronized object exists even when the observed stream had
            // no preceding work. Preserve it independently of source DAG edges.
            SourceStreamWait object;
            const auto api = record.at("api").get<std::string>();
            const bool device_wide = api == "rtDeviceSynchronize" || api == "rtDeviceSynchronizeWithTimeout";
            object.scope = device_wide ? SynchronizationScope::Device : SynchronizationScope::Stream;
            std::vector<Identity> synchronized_streams;
            if (device_wide) {
                for (const auto & [stream, lanes] : stream_lanes)
                    if (stream.first == thread.first) synchronized_streams.push_back(stream);
            }
            else if (!record.at("stream_id").is_null()) {
                object.native_stream = record.at("stream_id").get<std::uint64_t>();
                synchronized_streams.emplace_back(thread.first, *object.native_stream);
            }

            bool resolved = record.at("result") == 0 && synchronized_calls.contains(index);
            for (const auto & stream : synchronized_streams) {
                for (const auto device_lane : stream_lanes[stream]) {
                    resolved = resolved && lane_streams.at(device_lane).size() == 1;
                    object.lanes.push_back(device_lane);
                }
            }
            if (!resolved) {
                object.lanes.clear();
                graph_.observe_stream_wait(wait, std::move(object));
                continue;
            }

            const auto & sync = intervals[synchronized_calls.at(index)];
            const auto sync_begin = sync.at("start_ns").get<std::int64_t>();
            const auto sync_end = sync.at("end_ns").get<std::int64_t>();
            struct Frontier {
                std::size_t position;
                std::vector<SourceSpanId> spans;
            };
            std::map<std::size_t, Frontier> frontiers;
            std::size_t overlapping = 0;
            std::size_t unmapped_tasks = 0;
            for (const auto & stream : synchronized_streams) {
                unmapped_tasks += unmapped[stream];
                for (const auto & submitted : submissions[stream]) {
                    // A returned CANN submission precedes synchronization in the
                    // same profiler clock domain. Device start/end times do not
                    // decide whether a task was submitted to this stream.
                    if (submitted.end_ns > sync_begin) {
                        if (submitted.begin_ns < sync_end) ++overlapping;
                        continue;
                    }
                    const auto [chain, position] = device_order_.at(submitted.hardware);
                    auto & frontier = frontiers.try_emplace(chain, Frontier{ position, {} }).first->second;
                    if (position > frontier.position) frontier = { position, {} };
                    if (position == frontier.position) frontier.spans.push_back(submitted.hardware);

                    // Retain contradictions even for transitively covered tasks.
                    const auto source_end = graph_.spans()[submitted.hardware].end;
                    const auto lateness_ns = graph_.boundaries()[source_end].observed_time_ns - end_ns;
                    if (lateness_ns > 0)
                        gaps_.push_back({
                            {                          "kind",                                               "observation" },
                            {                        "reason",                    "native_host_wait_clock_order_disagrees" },
                            {               "source_interval",                                          submitted.hardware },
                            {           "submission_interval",                                               submitted.cpu },
                            {                      "wait_api",                                                       index },
                            {        "profiler_wait_interval",                                synchronized_calls.at(index) },
                            { "profiler_observed_lateness_ns", graph_.boundaries()[source_end].observed_time_ns - sync_end },
                            {          "observed_lateness_ns",                                                 lateness_ns }
                        });
                }
            }
            if (unmapped_tasks != 0 || overlapping != 0) {
                // A partial device domain must not become an idle resource query.
                if (device_wide) object.lanes.clear();
                gaps_.push_back({
                    {                      "kind",                                  "observation" },
                    {                    "reason", "native_host_wait_submission_frontier_partial" },
                    {                  "wait_api",                                          index },
                    { "unmapped_stream_intervals",                                 unmapped_tasks },
                    {   "overlapping_submissions",                                    overlapping }
                });
            }

            graph_.observe_stream_wait(wait, std::move(object));

            // Earlier stream tasks reach this frontier through existing task
            // completion/order edges; preserve all endpoints at each position.
            for (const auto & [chain, frontier] : frontiers) {
                for (const auto span : frontier.spans)
                    graph_.connect(graph_.spans()[span].end, graph_.spans()[wait].end, SourceRelation::NativeHostWait, index);
            }
        }
    }
}

void AscendSourceExtractor::complete_device_work() {
    std::vector<std::vector<SourceSpanId>> work(calls_.size());
    for (SourceSpanId identity = 0; identity < graph_.spans().size(); ++identity) {
        const auto & span = graph_.spans()[identity];
        const auto & lane = graph_.lanes()[graph_.boundaries()[span.begin].lane];
        if (span.owner_call && lane.domain == SourceDomain::DeviceStream) work[*span.owner_call].push_back(identity);
    }
    for (std::size_t index = 0; index < calls_.size(); ++index) {
        auto & call = calls_[index];
        if (!call.marker)
            gaps_.push_back({
                {   "kind",                "observation" },
                { "reason", "native_call_marker_missing" },
                {   "call",                call.identity }
            });
        if (call.is_annotation() || (call.step != StepKind::Prefill && call.step != StepKind::Decode)) continue;

        if (work[index].empty()) {
            gaps_.push_back({
                {   "kind",                        "observation" },
                { "reason", "execution_has_no_owned_device_work" },
                {   "call",                        call.identity }
            });
            continue;
        }
        call.device_done = graph_.complete(work[index], call.rank, index);
    }
}

nlohmann::json AscendSourceExtractor::read(nlohmann::json facts) {
    facts_ = std::move(facts);
    read_calls(facts_.at("calls"));
    read_intervals(facts_.at("intervals"));
    read_cpu_scopes();
    read_flows(facts_.at("flows"));
    read_device_order(facts_.at("intervals"), facts_.at("native_api"));
    read_communication(facts_.at("intervals"));

    read_operations(facts_.at("intervals"), facts_.at("flows"));
    read_event_signals(facts_.at("native_api"), facts_.at("intervals"));
    read_host_waits(facts_.at("native_api"), facts_.at("intervals"));
    read_copy_boundaries(facts_.at("native_api"), facts_.at("intervals"));

    // CPU segmentation needs the flow endpoints and explicit waits collected above.
    read_cpu_service_boundaries();
    extract_cpu_control();
    extract_worker_cpu();
    if (const auto unresolved = read_transfers(facts_.at("intervals"), facts_.at("native_api")))
        gaps_.push_back({
            {      "kind",                    "observation" },
            {    "reason", "transfer_parameters_unresolved" },
            { "intervals",                       unresolved }
        });

    read_cpu_boundaries(facts_.at("intervals"), facts_.at("flows"), facts_.at("native_api"));
    complete_device_work();

    const auto summary = graph_.seal();

    const bool acyclic = summary.visited_boundaries == summary.boundaries;
    if (acyclic) {
        subgraphs_ = BlockExtractor(graph_, calls_, bindings_).extract();
        for (const auto & gap : subgraphs_.gaps) gaps_.push_back(gap);
    }
    else {
        gaps_.push_back({
            {                 "kind",                                   "observation" },
            {               "reason",             "source_dependencies_contain_cycle" },
            { "unvisited_boundaries", summary.boundaries - summary.visited_boundaries }
        });
    }

    return {
        {                    "boundaries",             summary.boundaries },
        {                         "spans",                  summary.spans },
        {                         "edges",                  summary.edges },
        {                       "acyclic",                        acyclic },
        {           "flow_time_reversals",    summary.flow_time_reversals },
        {         "native_notify_signals",         summary.notify_signals },
        {          "native_event_signals",          summary.event_signals },
        { "native_host_wait_dependencies", summary.host_wait_dependencies },
        {           "dependency_coverage",                      "partial" },
        {                   "costs_bound",                          false }
    };
}

nlohmann::json AscendSourceExtractor::extract(const std::filesystem::path & manifest_path, nlohmann::json manifest,
                                              const std::filesystem::path & work_directory, bool diagnostics) {
    if (manifest.at("role") != "base") throw std::invalid_argument("source extraction requires a role=base manifest");

    const auto directory = manifest_path.parent_path();
    auto coverage = read(read_json(directory / manifest.at("source_facts").get<std::string>()));

    // Cuts and calls have one in-memory representation. The optional source
    // graph export is diagnostic and is never reopened by template production.
    manifest.erase("source_graph");
    if (diagnostics) {
        const auto graph_path = work_directory / "source_graph.json";
        graph_.write(graph_path);
        manifest["source_graph"] = std::filesystem::relative(graph_path, directory).string();
    }

    // Normalization keeps its own gaps. Re-extraction replaces only the
    // conclusions owned by this stage, without a placeholder handoff.
    manifest["extraction_gaps"] = gaps_;
    manifest["readiness"] = "needs_data";
    manifest["graph_coverage"] = std::move(coverage);

    return manifest;
}

} // namespace markov
