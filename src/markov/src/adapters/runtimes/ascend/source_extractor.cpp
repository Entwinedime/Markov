#include "markov/adapters/runtimes/ascend/source_extractor.hpp"

#include "markov/production/block_binding.hpp"
#include "markov/production/block_extractor.hpp"
#include "markov/production/source_bindings.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace markov {
namespace {

nlohmann::json read_json(const std::filesystem::path & path) {
    std::ifstream stream(path);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    return nlohmann::json::parse(stream);
}

void write_json(const std::filesystem::path & path, const nlohmann::json & value) {
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream stream(temporary);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream << value.dump(2) << '\n';

    stream.close();

    std::filesystem::rename(temporary, path);
}

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
        SourceCall call{ identity,
                         step_kind(name),
                         name == "model_forward"                                  ? SourceCallKind::ForwardInputs
                         : (name == "model_region" || name == "component_region") ? SourceCallKind::NativeRegion
                                                                                  : SourceCallKind::Serving,
                         record.at("rank").get<std::uint32_t>(),
                         std::nullopt,
                         std::nullopt,
                         std::nullopt,
                         {},
                         std::nullopt,
                         record.at("thread_cpu_ns").get<std::int64_t>(),
                         record.at("start_ns").get<std::int64_t>(),
                         std::nullopt,
                         std::nullopt,
                         {},
                         {},
                         std::nullopt,
                         {},
                         std::nullopt };

        const auto & binding = record.at("binding");
        if (!binding.is_null()) call.binding = binding.get<std::string>();

        const auto & metadata = record.at("metadata");
        if (name == "schedule") {
            call.visible_requests = metadata.at("visible_requests").get<std::vector<std::string>>();
            const auto policy = metadata.find("admission_policy");
            if (policy != metadata.end()) { call.admission_policy = SourceAdmissionPolicy::read(*policy); }
            const auto controls = metadata.find("worker_controls");
            if (controls != metadata.end()) call.worker_controls = SourceWorkerControls::read(*controls);

            for (const auto & filter : record.at("result").at("batch_filters")) {
                call.batch_filters.push_back({ filter.at("before").get<std::vector<std::string>>(), filter.at("after").get<std::vector<std::string>>() });
            }
        }
        if (name == "publish") {
            const auto policy = record.at("result").find("output_policy");
            if (policy != record.at("result").end()) {
                const auto interval = policy->at("force_interval_tokens").get<std::int64_t>();
                if (interval <= 0 || interval > std::numeric_limits<std::uint32_t>::max())
                    throw std::invalid_argument("native output interval must be a positive uint32 token count");
                call.output_policy = SourceOutputPolicy{ static_cast<std::uint32_t>(interval), policy->at("non_streaming").get<bool>() };
            }

            for (const auto & request : record.at("result").at("requests")) {
                call.publications.push_back(
                    { request.at("id").get<std::string>(), request.at("generated_tokens").get<std::uint32_t>(), request.at("finished").get<bool>() });
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

        if (call.kind == SourceCallKind::ForwardInputs) {
            call.input_layout = SourceTensorLayout{ metadata.at("input_shape").get<std::vector<std::int64_t>>(),
                                                    metadata.at("input_stride").get<std::vector<std::int64_t>>(),
                                                    metadata.at("input_dtype").get<std::string>(),
                                                    metadata.at("input_device").get<std::string>() };
        }

        if (call.kind == SourceCallKind::NativeRegion) {
            call.region = bindings_.region(call.binding.value()).read(metadata);
            // Preserve native outcomes as evidence. Framework producers decide
            // their semantics; absent work fields do not become zero counts.
            call.region->result = record.at("result").get<nlohmann::json::object_t>();
        }

        call_ids_.emplace(identity, calls_.size());
        calls_.push_back(std::move(call));
    }

    // Resolve parent identities after every call has entered the lookup table.
    for (auto & call : calls_) {
        const auto & parent = records.at(call.identity).at("parent_id");
        if (!parent.is_null()) { call.parent = call_ids_.at(std::to_string(call.rank) + ":" + std::to_string(parent.get<std::uint64_t>())); }
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
        if (call.kind == SourceCallKind::ForwardInputs) calls_[parent.value()].input_layout = call.input_layout;
    }
}

void AscendSourceExtractor::read_intervals(const nlohmann::json & intervals) {
    std::size_t collective_roots = 0;
    std::map<std::pair<std::size_t, std::size_t>, std::vector<SourceSpanId>> operator_intervals;
    for (const auto & interval : intervals) {
        const std::size_t identity = interval.at("id").get<std::size_t>();
        const auto begin_ns = interval.at("start_ns").get<std::int64_t>();
        const auto end_ns = interval.at("end_ns").get<std::int64_t>();
        if (identity != graph_.spans().size() || end_ns < begin_ns) {
            throw std::invalid_argument("source interval identity/order or duration is invalid at " + std::to_string(identity));
        }

        const SourceSpanKind kind = span_kind(interval.at("kind").get<std::string>());
        SourceDomain domain = SourceDomain::DeviceStream;
        if (kind == SourceSpanKind::CpuObservation) domain = SourceDomain::CpuThread;
        if (kind == SourceSpanKind::CommunicationObservation) domain = SourceDomain::CommunicationObservation;

        const auto rank = interval.at("rank").get<std::uint32_t>();
        const auto native_id = interval.at("tid").get<std::uint64_t>();
        const auto lane = graph_.lane(domain, rank, native_id);
        std::optional<std::size_t> owner;
        if (!interval.at("owner_call").is_null()) { owner = call_ids_.at(interval.at("owner_call").get<std::string>()); }
        std::optional<std::size_t> scope;
        if (!interval.at("scope_call").is_null()) scope = call_ids_.at(interval.at("scope_call").get<std::string>());
        const auto span_id = graph_.observe(kind, lane, begin_ns, end_ns, owner, identity, std::nullopt, std::nullopt, scope);

        const std::string name = interval.at("name").get<std::string>();
        if (kind == SourceSpanKind::CpuObservation && (name == "MarkovWorkspaceAllocate" || name == "MarkovWorkspaceRelease")) {
            // Allocation is a replaceable resource operation inside a worker.
            // Its marker must cut CPU service too, not only annotate the graph.
            cpu_service_boundaries_[lane].insert(begin_ns);
            cpu_service_boundaries_[lane].insert(end_ns);
        }
        if (kind == SourceSpanKind::CpuObservation && owner && scope && name.starts_with("aten::")) operator_intervals[{ lane, *scope }].push_back(span_id);
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

        if (name.starts_with("markov/")) {
            const auto separator = name.find('/', 7);
            const auto call = call_ids_.at(std::to_string(rank) + ":" + name.substr(separator + 1));
            calls_[call].marker = span_id;
        }

        if (kind == SourceSpanKind::CommunicationObservation && name.starts_with("hcom_")) ++collective_roots;
    }

    // A backend launch may be nested in an ATen operation that also allocates
    // or copies storage. Delimit the outer operation, not each internal helper.
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
            const auto & span = graph_.spans()[identity];
            const auto begin = graph_.boundaries()[span.begin].observed_time_ns;
            const auto end = graph_.boundaries()[span.end].observed_time_ns;
            if (outer_end && begin < *outer_end) {
                if (end > *outer_end) throw std::invalid_argument("CPU operator observations overlap without nesting");
                continue;
            }
            cpu_service_boundaries_[location.first].insert(begin);
            cpu_service_boundaries_[location.first].insert(end);
            cpu_operation_roots_[location].push_back(identity);
            outer_end = end;
        }
    }

    gaps_.push_back({
        {   "kind",                                         "cost" },
        { "reason", "cpu_service_and_probe_overhead_not_separated" }
    });
    if (collective_roots != 0)
        gaps_.push_back({
            {      "kind",                                       "cost" },
            {    "reason", "collective_transport_service_not_separated" },
            { "intervals",                             collective_roots }
        });
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
    std::map<SourceSpanId, const SourceCollective *> collectives;
    for (const auto & collective : graph_.collectives()) collectives.emplace(collective.span, &collective);
    // A view-only branch still performs CPU work. Operation identity must not
    // depend on whether this invocation happened to submit a device task.
    for (const auto & [location, roots] : cpu_operation_roots_) {
        for (const auto host : roots) operations.emplace(host, SourceOperation{ intervals[host].at("name").get<std::string>(), host, {}, {} });
    }

    for (const auto & flow : flows) {
        if (flow.at("category") != "async_npu" || flow.at("source_intervals").size() != 1) continue;
        const auto call = flow.at("source_intervals")[0].get<SourceSpanId>();
        const auto & span = graph_.spans()[call];
        if (span.kind != SourceSpanKind::CpuObservation || !span.owner_call || !span.scope_call) continue;
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
            for (const auto & task : collective->second->tasks) position->second.device_spans.push_back(task.span);
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
        if (!source.scope_call || source.scope_call != worker.scope_call || source.owner_call != worker.owner_call) continue;
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
        // The vendor flow is anchored at API/queue start, not at API return.
        // Connecting return to device start would manufacture a CPU/NPU barrier.
        for (const auto & source : flow.at("source_intervals")) {
            const auto source_id = source.get<SourceSpanId>();
            for (const auto & target : flow.at("target_intervals")) {
                const auto target_id = target.get<SourceSpanId>();
                graph_.connect(graph_.spans().at(source_id).begin, graph_.spans().at(target_id).begin, SourceRelation::NativeFlow, index);

                for (const auto endpoint : { source_id, target_id }) {
                    const auto & span = graph_.spans()[endpoint];
                    if (span.kind != SourceSpanKind::CpuObservation) continue;
                    const auto & boundary = graph_.boundaries()[span.begin];
                    cpu_service_boundaries_[boundary.lane].insert(boundary.observed_time_ns);
                    // The call's return delimits its CPU service for a later
                    // operation replacement; launch ordering still uses begin.
                    cpu_service_boundaries_[boundary.lane].insert(graph_.boundaries()[span.end].observed_time_ns);
                }
            }
        }
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
        if (record.at("api") != "rtGeneralCtrl" || record.at("result") != 0 || record.at("submitted_task_id").is_null()
            || record.at("submitted_stream_id") != record.at("stream_id"))
            continue;

        submissions[{ record.at("rank").get<std::uint32_t>(),
                      record.at("submitted_stream_id").get<std::uint64_t>(),
                      record.at("submitted_task_id").get<std::uint64_t>() }]
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
            observations.push_back(
                { interval.at("id").get<SourceSpanId>(),
                  rank,
                  connection,
                  interval.at("name").get<std::string>(),
                  args.at("count").get<std::uint64_t>(),
                  args.at("data_type").get<std::string>(),
                  args.at("alg_type").get<std::string>(),
                  args.at("group_name").get<std::string>(),
                  interval.at("communicator").is_null() ? std::nullopt : std::optional(SourceCommunicator::read(interval.at("communicator"))),
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
        if (physical.size() != 1 || args.at("src rank").get<std::uint32_t>() != rank) {
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
        const auto producer = name == "Notify_Record" ? rank : peer;
        const auto consumer = name == "Notify_Record" ? peer : rank;
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

std::expected<std::vector<SourceSpanId>, std::string> AscendSourceExtractor::event_wait_frontier(const nlohmann::json & record,
                                                                                                 const nlohmann::json & intervals) const {
    if (record.at("source_interval").is_null() || record.at("stream_id").is_null()) return std::unexpected("event_wait_submission_not_observed");
    const auto & wait = intervals.at(record.at("source_interval").get<std::size_t>());
    const auto rank = record.at("rank").get<std::uint32_t>();
    const auto stream = record.at("stream_id").get<std::uint64_t>();
    std::map<std::uint64_t, std::vector<SourceSpanId>> submissions;
    for (const auto & interval : intervals) {
        if (interval.at("rank") == rank && interval.at("process") == "CANN")
            submissions[interval.at("arguments").at("connection_id").get<std::uint64_t>()].push_back(interval.at("id").get<SourceSpanId>());
    }

    // Only same-thread submission order qualifies this projection. Device
    // timestamps and the last returned task ID cannot identify a skipped wait.
    struct Frontier {
        std::size_t position;
        std::vector<SourceSpanId> spans;
    };
    std::map<std::size_t, Frontier> after;
    std::map<std::size_t, std::size_t> before;
    for (const auto & interval : intervals) {
        if (interval.at("rank") != rank || interval.at("process") != "Ascend Hardware" || interval.at("arguments").at("Physic Stream Id") != stream) continue;
        const auto & matches = submissions[interval.at("arguments").at("connection_id").get<std::uint64_t>()];
        if (matches.size() != 1) return std::unexpected("event_wait_stream_submission_not_unique");
        const auto & submission = intervals.at(matches.front());
        if (submission.at("tid") != wait.at("tid")) return std::unexpected("event_wait_concurrent_submission_not_qualified");

        const auto span = interval.at("id").get<SourceSpanId>();
        const auto [chain, position] = device_order_.at(span);
        if (submission.at("end_ns") <= wait.at("start_ns")) {
            auto previous = before.try_emplace(chain, position).first;
            previous->second = std::max(previous->second, position);
        }
        else if (submission.at("start_ns") >= wait.at("end_ns")) {
            auto & frontier = after.try_emplace(chain, Frontier{ position, {} }).first->second;
            if (position < frontier.position) frontier = { position, {} };
            if (position == frontier.position) frontier.spans.push_back(span);
        }
        else return std::unexpected("event_wait_overlapping_submission_not_qualified");
    }
    if (after.empty()) return std::unexpected("event_wait_consumer_frontier_not_observed");

    std::map<BoundaryId, SourceSpanId> entries;
    std::set<BoundaryId> ends;
    for (const auto & [chain, frontier] : after) {
        const auto previous = before.find(chain);
        if (previous != before.end() && previous->second >= frontier.position) return std::unexpected("event_wait_submission_order_disagrees");
        for (const auto span : frontier.spans) {
            entries.emplace(graph_.spans()[span].begin, span);
            ends.insert(graph_.spans()[span].end);
        }
    }

    // A compound launch may fork. Gate every native dependency root; internal
    // descriptor edges carry the event condition to the remaining sub-tasks.
    for (const auto & edge : graph_.edges()) {
        if (edge.relation == SourceRelation::NativeTaskDependency && ends.contains(edge.source)) entries.erase(edge.target);
    }
    std::vector<SourceSpanId> frontier;
    for (const auto & [boundary, span] : entries) frontier.push_back(span);
    return frontier;
}

void AscendSourceExtractor::read_event_signals(const nlohmann::json & records, const nlohmann::json & intervals) {
    struct Signal {
        std::size_t api;
        std::int64_t returned_ns;
        std::vector<SourceSpanId> spans;
    };
    using EventKey = std::pair<std::uint64_t, std::uint64_t>; // Event handles are process-local and may be reused after destroy.
    std::map<EventKey, Signal> signals;
    std::set<SourceSpanId> unresolved;
    for (const auto & interval : intervals) {
        if (interval.at("process") == "Ascend Hardware" && interval.at("name") == "EVENT_WAIT") { unresolved.insert(interval.at("id").get<SourceSpanId>()); }
    }

    native_api_members_.resize(calls_.size());
    std::vector<std::size_t> sequence(records.size());
    std::iota(sequence.begin(), sequence.end(), 0);
    std::sort(sequence.begin(), sequence.end(), [&records](std::size_t left, std::size_t right) {
        return records[left].at("start_ns").get<std::int64_t>() < records[right].at("start_ns").get<std::int64_t>();
    });

    std::map<EventKey, std::vector<std::int64_t>> changes;
    for (const auto index : sequence) {
        const auto & record = records[index];
        const auto name = record.at("api").get<std::string>();
        if (record.at("result").get<std::int32_t>() == 0 && (name == "rtEventRecord" || name == "rtEventDestroy")) {
            changes[EventKey(record.at("pid").get<std::uint64_t>(), record.at("event_handle").get<std::uint64_t>())].push_back(
                record.at("start_ns").get<std::int64_t>());
        }
    }
    for (const auto index : sequence) {
        const auto & record = records[index];
        if (!record.at("owner_call").is_null()) { native_api_members_[call_ids_.at(record.at("owner_call").get<std::string>())].push_back(index); }
        if (record.at("result").get<std::int32_t>() != 0) continue;

        const auto name = record.at("api").get<std::string>();
        const EventKey key(record.at("pid").get<std::uint64_t>(), record.at("event_handle").get<std::uint64_t>());
        if (name == "rtEventDestroy") {
            signals.erase(key);
            continue;
        }
        if (name == "rtEventRecord") {
            signals.insert_or_assign(
                key,
                Signal{ index, record.at("end_ns").get<std::int64_t>(), record.at("hardware_intervals").get<std::vector<SourceSpanId>>() });
            continue;
        }
        if (name != "rtStreamWaitEvent") continue;

        const auto producer = signals.find(key);
        // A completed record call before wait entry identifies its captured
        // generation. Concurrent record/wait calls need another causal fact;
        // timestamps alone cannot pick a generation or recover a missing task.
        if (producer == signals.end() || producer->second.returned_ns > record.at("start_ns").get<std::int64_t>() || producer->second.spans.empty()) continue;

        const auto & mutations = changes[key];
        const auto concurrent = std::lower_bound(mutations.begin(), mutations.end(), record.at("start_ns").get<std::int64_t>());
        if (concurrent != mutations.end() && *concurrent < record.at("end_ns").get<std::int64_t>()) continue;

        if (record.at("hardware_intervals").empty()) {
            const auto frontier = event_wait_frontier(record, intervals);
            if (!frontier) {
                gaps_.push_back({
                    {     "kind",    "observation" },
                    {   "reason", frontier.error() },
                    { "wait_api",            index }
                });
                continue;
            }
            // A successful wait gates later stream work even when no separate
            // hardware wait was recorded. Preserve the API contract as edges,
            // without inventing a service interval or a measured duration.
            for (const auto target : *frontier) {
                for (const auto source : producer->second.spans)
                    graph_.connect(graph_.spans()[source].end, graph_.spans()[target].begin, SourceRelation::NativeEventSignal, producer->second.api);
            }
            continue;
        }

        for (const auto wait : record.at("hardware_intervals").get<std::vector<SourceSpanId>>()) {
            for (const auto source : producer->second.spans) {
                graph_.connect(graph_.spans()[source].end, graph_.spans()[wait].end, SourceRelation::NativeEventSignal, producer->second.api);
            }
            unresolved.erase(wait);
        }
    }

    if (!unresolved.empty()) {
        gaps_.push_back({
            {      "kind",                                 "observation" },
            {    "reason", "native_event_wait_producer_identity_missing" },
            { "intervals",                             unresolved.size() }
        });
    }
}

void AscendSourceExtractor::read_copy_boundaries(const nlohmann::json & records, const nlohmann::json & intervals) {
    auto unresolved_prefixes = nlohmann::json::array();
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto & record = records[index];
        if (record.at("copy_execution") != "synchronous_driver") continue;
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
        if (interval.at("process") != "CANN" || interval.contains("profiler_control")) continue;
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
        if (!record.contains("profiler_control") && names.contains(record.at("api").get<std::string>())) {
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
        if (record.contains("profiler_control") || record.at("copy_execution") != "synchronous_driver" || record.at("transfer_kind") != 1
            || record.at("source_interval").is_null())
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
            const auto wait = graph_.observe(SourceSpanKind::HostWait, lane, begin_ns, end_ns, owner, std::nullopt, std::nullopt, index, scope);
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

void AscendSourceExtractor::observe_cpu_control(std::size_t lane, std::int64_t begin_ns, std::int64_t end_ns, std::size_t owner,
                                                std::optional<std::size_t> source_interval, SourceCpuPart part, std::optional<std::size_t> recorded_scope) {
    std::vector<std::pair<std::int64_t, std::int64_t>> waits;
    for (const auto identity : host_waits_) {
        const auto & wait = graph_.spans()[identity];
        const auto & begin = graph_.boundaries()[wait.begin];
        const auto & end = graph_.boundaries()[wait.end];
        if (begin.lane != lane || begin.observed_time_ns >= end_ns || end.observed_time_ns <= begin_ns) continue;
        waits.emplace_back(std::max(begin_ns, begin.observed_time_ns), std::min(end_ns, end.observed_time_ns));
    }

    // Waiting is a dependency, so remove its extent from CPU service candidates.
    std::sort(waits.begin(), waits.end());
    std::vector<std::pair<std::int64_t, std::int64_t>> segments;
    auto cursor_ns = begin_ns;
    for (const auto & [wait_begin, wait_end] : waits) {
        if (cursor_ns < wait_begin) segments.emplace_back(cursor_ns, wait_begin);
        cursor_ns = std::max(cursor_ns, wait_end);
    }
    if (cursor_ns < end_ns) segments.emplace_back(cursor_ns, end_ns);

    // Preserve both submission cuts and native scope changes. Nested annotations
    // partition this service once; they never introduce another service charge.
    const auto & cuts = cpu_service_boundaries_[lane];
    const auto & scopes = cpu_scope_changes_[lane];
    for (const auto & [begin, end] : segments) {
        cursor_ns = begin;
        auto point = cuts.upper_bound(begin);
        while (cursor_ns < end) {
            const auto next_ns = point == cuts.end() ? end : std::min(end, *point);
            const auto change = scopes.upper_bound(cursor_ns);
            auto scope = recorded_scope;
            if (!scope && change != scopes.begin()) scope = std::prev(change)->second;
            graph_.observe(SourceSpanKind::CpuControl, lane, cursor_ns, next_ns, owner, source_interval, part, std::nullopt, scope);
            cursor_ns = next_ns;
            if (point != cuts.end()) ++point;
        }
    }
}

void AscendSourceExtractor::extract_cpu_control() {
    for (std::size_t index = 0; index < calls_.size(); ++index) {
        const auto & call = calls_[index];
        if (call.is_annotation() || !call.marker) continue;

        // Copies survive observe() growing the graph's span storage.
        const SourceSpan marker = graph_.spans()[*call.marker];
        const auto lane = graph_.boundaries()[marker.begin].lane;
        const auto begin_ns = graph_.boundaries()[marker.begin].observed_time_ns;
        const auto end_ns = graph_.boundaries()[marker.end].observed_time_ns;

        if (call.step == StepKind::Schedule) {
            auto selection_ns = end_ns;
            for (const auto & child : calls_) {
                if (!child.is_annotation() && child.serving_parent(calls_) == index && child.step == StepKind::Prepare && child.marker) {
                    selection_ns = graph_.boundaries()[graph_.spans()[*child.marker].begin].observed_time_ns;
                }
            }
            observe_cpu_control(lane, begin_ns, selection_ns, index, marker.source_interval, SourceCpuPart::SchedulePrefix);
            continue;
        }

        if (call.step == StepKind::Prepare) {
            observe_cpu_control(lane, begin_ns, end_ns, index, marker.source_interval, SourceCpuPart::PrepareBody);
            const auto schedule = call.serving_parent(calls_);
            if (schedule && calls_[*schedule].marker) {
                const SourceSpan parent = graph_.spans()[*calls_[*schedule].marker];
                const auto returned_ns = graph_.boundaries()[parent.end].observed_time_ns;
                observe_cpu_control(lane, end_ns, returned_ns, index, parent.source_interval, SourceCpuPart::PrepareSuffix);
            }
            continue;
        }

        const auto part = call.step == StepKind::Publish ? SourceCpuPart::Publish : SourceCpuPart::Execute;
        observe_cpu_control(lane, begin_ns, end_ns, index, marker.source_interval, part);
    }
}

void AscendSourceExtractor::bind_worker_cpu(const nlohmann::json & intervals, const nlohmann::json & flows) {
    std::map<std::pair<BoundaryId, BoundaryId>, SourceSpanId> submissions;
    for (const auto & flow : flows) {
        if (flow.value("source_binding", "") != "native_flow_runtime_submission") continue;
        const auto identity = flow.at("source_intervals").at(0).get<SourceSpanId>();
        const auto & span = graph_.spans()[identity];
        submissions.emplace(std::pair{ span.begin, span.end }, identity);
    }

    std::map<std::size_t, std::vector<SourceSpanId>> services;
    for (SourceSpanId identity = 0; identity < graph_.spans().size(); ++identity) {
        const auto & span = graph_.spans()[identity];
        if (span.kind == SourceSpanKind::CpuControl && span.control_part == SourceCpuPart::Dispatch && span.source_interval)
            services[*span.source_interval].push_back(identity);
    }

    for (auto & [worker, members] : services) {
        const auto name = intervals.at(worker).at("name").get<std::string>();
        if (!name.starts_with("Dequeue@") || members.size() != 3) continue;
        std::ranges::sort(members, [&](SourceSpanId left, SourceSpanId right) {
            return graph_.boundaries()[graph_.spans()[left].begin].observed_time_ns < graph_.boundaries()[graph_.spans()[right].begin].observed_time_ns;
        });

        const auto & before = graph_.spans()[members[0]];
        const auto & call = graph_.spans()[members[1]];
        const auto & after = graph_.spans()[members[2]];
        const auto & window = graph_.spans()[worker];
        if (before.begin != window.begin || before.end != call.begin || call.end != after.begin || after.end != window.end) continue;
        const auto submission = submissions.find({ call.begin, call.end });
        if (submission == submissions.end()) continue;

        // Bind existing complete services only. Extra waits or cuts are neither
        // removed nor assigned a fraction of a whole-worker measurement.
        const auto operation = name.substr(std::string_view("Dequeue@").size());
        graph_.observe_cpu_work(members[0], { operation, CpuSubmissionPart::Before, submission->second });
        graph_.observe_cpu_work(members[1], { operation, CpuSubmissionPart::Call, submission->second });
        graph_.observe_cpu_work(members[2], { operation, CpuSubmissionPart::After, submission->second });
    }
}

void AscendSourceExtractor::extract_worker_cpu() {
    std::set<std::size_t> control_lanes;

    for (const auto & call : calls_) {
        if (call.marker) control_lanes.insert(graph_.boundaries()[graph_.spans()[*call.marker].begin].lane);
    }

    struct Endpoint {
        std::int64_t time_ns;
        SourceSpanId span;
        bool entering;
    };
    struct Coverage {
        std::int64_t begin_ns;
        std::int64_t end_ns;
        std::size_t owner;
        std::optional<std::size_t> source_interval;
        std::optional<std::size_t> scope;
    };
    std::map<std::size_t, std::vector<Endpoint>> lanes;
    for (SourceSpanId identity = 0; identity < graph_.spans().size(); ++identity) {
        const auto & span = graph_.spans()[identity];
        if (span.kind != SourceSpanKind::CpuObservation || !span.owner_call) continue;
        const auto & begin = graph_.boundaries()[span.begin];
        const auto & end = graph_.boundaries()[span.end];
        if (control_lanes.contains(begin.lane) || begin.observed_time_ns == end.observed_time_ns) continue;
        lanes[begin.lane].push_back({ begin.observed_time_ns, identity, true });
        lanes[begin.lane].push_back({ end.observed_time_ns, identity, false });
    }

    for (auto & [lane, endpoints] : lanes) {
        std::sort(endpoints.begin(), endpoints.end(), [](const Endpoint & left, const Endpoint & right) { return left.time_ns < right.time_ns; });
        std::set<SourceSpanId> active;
        std::map<std::size_t, std::size_t> owners;
        std::map<std::optional<std::size_t>, std::size_t> scopes;
        std::size_t conflicts = 0;
        auto previous_ns = endpoints.front().time_ns;
        std::size_t position = 0;
        std::optional<Coverage> coverage;

        while (position < endpoints.size()) {
            const auto time_ns = endpoints[position].time_ns;
            if (previous_ns < time_ns && owners.size() == 1) {
                // Nested annotations describe the same worker time. Retain one
                // covered interval, not a service charge for each annotation.
                const auto source_interval = graph_.spans()[*active.begin()].source_interval;
                // Preserve the worker observations' explicit identity only when
                // all covering annotations agree, including unknown identities.
                const auto scope = scopes.size() == 1 ? scopes.begin()->first : std::nullopt;
                const auto owner = owners.begin()->first;
                if (coverage && coverage->end_ns == previous_ns && coverage->owner == owner && coverage->source_interval == source_interval
                    && coverage->scope == scope) {
                    coverage->end_ns = time_ns;
                }
                else {
                    if (coverage)
                        observe_cpu_control(lane,
                                            coverage->begin_ns,
                                            coverage->end_ns,
                                            coverage->owner,
                                            coverage->source_interval,
                                            SourceCpuPart::Dispatch,
                                            coverage->scope);
                    coverage = Coverage{ previous_ns, time_ns, owner, source_interval, scope };
                }
            }
            else if (previous_ns < time_ns && owners.size() > 1) ++conflicts;

            // Update all coincident endpoints before attributing the next interval.
            while (position < endpoints.size() && endpoints[position].time_ns == time_ns) {
                const auto & endpoint = endpoints[position++];
                const auto owner = *graph_.spans()[endpoint.span].owner_call;
                const auto scope = graph_.spans()[endpoint.span].scope_call;
                if (endpoint.entering) {
                    active.insert(endpoint.span);
                    ++owners[owner];
                    ++scopes[scope];
                }
                else {
                    active.erase(endpoint.span);
                    if (--owners.at(owner) == 0) owners.erase(owner);
                    if (--scopes.at(scope) == 0) scopes.erase(scope);
                }
            }
            previous_ns = time_ns;
        }

        // Nested observations remain in the graph. Only identical continuous
        // service coverage is combined; observe_cpu_control retains causal cuts.
        if (coverage)
            observe_cpu_control(lane,
                                coverage->begin_ns,
                                coverage->end_ns,
                                coverage->owner,
                                coverage->source_interval,
                                SourceCpuPart::Dispatch,
                                coverage->scope);

        if (conflicts != 0) {
            gaps_.push_back({
                {        "kind",                 "observation" },
                {      "reason", "cpu_dispatch_scope_conflict" },
                { "source_lane",                          lane },
                {    "segments",                     conflicts }
            });
        }
    }
}

void AscendSourceExtractor::complete_device_work() {
    std::vector<std::vector<SourceSpanId>> work(calls_.size());
    for (SourceSpanId identity = 0; identity < graph_.spans().size(); ++identity) {
        const auto & span = graph_.spans()[identity];
        if (span.owner_call && span.kind != SourceSpanKind::CpuObservation && span.kind != SourceSpanKind::CpuControl && span.kind != SourceSpanKind::HostWait
            && span.kind != SourceSpanKind::CommunicationObservation) {
            work[*span.owner_call].push_back(identity);
        }
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

nlohmann::json AscendSourceExtractor::extract(const std::filesystem::path & manifest_path, nlohmann::json manifest) {
    if (manifest.at("role") != "base") throw std::invalid_argument("source extraction requires a role=base manifest");

    const auto directory = manifest_path.parent_path();
    const auto environment = read_json(directory / manifest.at("environment").get<std::string>());
    native_dense_path_ = bindings_.supports_native_path(environment);

    const auto facts = read_json(directory / manifest.at("source_facts").get<std::string>());
    read_calls(facts.at("calls"));
    read_intervals(facts.at("intervals"));
    read_cpu_scopes();
    read_flows(facts.at("flows"));
    read_device_order(facts.at("intervals"), facts.at("native_api"));
    read_communication(facts.at("intervals"));

    read_operations(facts.at("intervals"), facts.at("flows"));
    read_event_signals(facts.at("native_api"), facts.at("intervals"));
    read_host_waits(facts.at("native_api"), facts.at("intervals"));
    read_copy_boundaries(facts.at("native_api"), facts.at("intervals"));

    // CPU segmentation needs the flow endpoints and explicit waits collected above.
    extract_cpu_control();
    extract_worker_cpu();
    bind_worker_cpu(facts.at("intervals"), facts.at("flows"));
    if (const auto unresolved = bindings_.bind_transfers(graph_, facts.at("intervals"), facts.at("native_api"), calls_))
        gaps_.push_back({
            {      "kind",                    "observation" },
            {    "reason", "transfer_parameters_unresolved" },
            { "intervals",                       unresolved }
        });

    complete_device_work();

    const auto summary = graph_.seal();

    const bool acyclic = summary.visited_boundaries == summary.boundaries;
    const auto graph_path = directory / "source_graph.json";
    const auto template_path = directory / "templates.json";
    graph_.write(graph_path);
    const auto templates = bindings_.extract_blocks(graph_, calls_, native_api_members_, native_dense_path_);
    write_json(template_path, templates);
    for (const auto & gap : templates.at("gaps")) gaps_.push_back(gap);

    auto collection_gaps = nlohmann::json::array();
    for (const auto & gap : manifest.at("gaps")) {
        if (gap.at("reason") != "source_dag_and_templates_not_extracted") collection_gaps.push_back(gap);
    }
    if (!acyclic) {
        gaps_.push_back({
            {                 "kind",                                   "observation" },
            {               "reason",             "source_dependencies_contain_cycle" },
            { "unvisited_boundaries", summary.boundaries - summary.visited_boundaries }
        });
    }

    manifest["source_graph"] = graph_path.filename().string();
    manifest["source_dag"] = acyclic ? nlohmann::json(graph_path.filename().string()) : nlohmann::json(nullptr);
    manifest["templates"] = template_path.filename().string();
    manifest.erase("compiled_templates");

    // Each stage owns its gap list. Re-extracting a captured base replaces this
    // stage's conclusions instead of appending its previous derived gaps again.
    manifest["gaps"] = collection_gaps;
    manifest["extraction_gaps"] = gaps_;
    manifest["readiness"] = "needs_data";
    manifest["graph_coverage"] = {
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

    write_json(manifest_path, manifest);
    for (const auto & gap : collection_gaps) gaps_.push_back(gap);

    return {
        {  "base_manifest",     manifest_path.string() },
        {      "readiness",               "needs_data" },
        { "graph_coverage", manifest["graph_coverage"] },
        {           "gaps",                      gaps_ }
    };
}

} // namespace markov
