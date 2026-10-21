#include "markov/adapters/runtimes/ascend/source_extractor.hpp"

#include "markov/adapters/runtimes/ascend/cpu_identity.hpp"
#include "markov/production/source_bindings.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace markov {

void AscendSourceExtractor::read_cpu_boundaries(const nlohmann::json & intervals, const nlohmann::json & flows, const nlohmann::json & records) {
    std::set<std::size_t> control_lanes;
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::size_t> lanes;
    for (std::size_t index = 0; index < graph_.lanes().size(); ++index) {
        const auto & lane = graph_.lanes()[index];
        if (lane.domain == SourceDomain::CpuThread) lanes.emplace(std::pair(lane.rank, lane.native_id), index);
    }
    for (const auto & call : calls_)
        if (call.marker) control_lanes.insert(graph_.boundaries()[graph_.spans()[*call.marker].begin].lane);

    // Name only existing service cuts. Instrumentation envelopes inside a
    // service interval must not create a new split or a new timing obligation.
    std::map<std::pair<std::size_t, std::int64_t>, BoundaryId> points;
    for (const auto & span : graph_.spans()) {
        if (span.kind != SourceSpanKind::CpuControl && span.kind != SourceSpanKind::HostWait) continue;
        for (const auto id : { span.begin, span.end }) {
            const auto & point = graph_.boundaries()[id];
            points.emplace(std::pair(point.lane, point.observed_time_ns), id);
        }
    }

    std::map<BoundaryId, std::pair<CpuBoundary, std::int64_t>> descriptions;
    const auto priority = [](CpuBoundaryKind kind) { return kind == CpuBoundaryKind::InternalWait ? CpuBoundaryKind::Wait : kind; };
    const auto retain = [&](std::size_t lane,
                            std::int64_t begin_ns,
                            std::int64_t end_ns,
                            CpuBoundaryKind kind,
                            const std::string & name,
                            std::size_t ordinal = 0,
                            std::size_t count = 1) {
        const auto role = control_lanes.contains(lane) ? CpuThreadRole::Control : CpuThreadRole::Worker;
        for (const auto edge : { CpuBoundaryEdge::Begin, CpuBoundaryEdge::End }) {
            const auto point = points.find({ lane, edge == CpuBoundaryEdge::Begin ? begin_ns : end_ns });
            if (point == points.end()) continue;

            CpuBoundary candidate{ role, kind, edge, name, ordinal, count };
            const auto previous = descriptions.find(point->second);
            // Prefer the causal/physical boundary over its enclosing wrapper;
            // equal kinds use the innermost native range deterministically.
            if (previous == descriptions.end() || priority(previous->second.first.kind) < priority(kind)
                || (previous->second.first.kind == kind && end_ns - begin_ns < previous->second.second))
                descriptions.insert_or_assign(point->second, std::pair(std::move(candidate), end_ns - begin_ns));
        }
    };
    const auto retain_span = [&](SourceSpanId id, CpuBoundaryKind kind, const std::string & name, std::size_t ordinal = 0, std::size_t count = 1) {
        const auto & span = graph_.spans()[id];
        const auto & begin = graph_.boundaries()[span.begin];
        retain(begin.lane, begin.observed_time_ns, graph_.boundaries()[span.end].observed_time_ns, kind, name, ordinal, count);
    };

    for (const auto & call : calls_)
        if (!call.is_annotation() && call.marker) retain_span(*call.marker, CpuBoundaryKind::Scope, std::string(ServingContract{}.block(call.step).name));

    for (const auto & interval : intervals) {
        const auto id = interval.at("id").get<SourceSpanId>();
        if (interval.at("kind") != "cpu" || observer_work(id)) continue;
        const auto & name = interval.at("name").get_ref<const std::string &>();
        if (interval.at("operator_function").get<bool>()) retain_span(id, CpuBoundaryKind::Function, name);
        if (name.starts_with("Runtime@")) retain_span(id, CpuBoundaryKind::Runtime, ascend_cpu::runtime_entry(name));
    }

    std::map<SourceSpanId, SourceSpanId> enqueues;
    for (const auto & flow : flows) {
        if (flow.at("category") == "async_task_queue" && flow.at("source_intervals").size() == 1 && flow.at("target_intervals").size() == 1)
            enqueues.emplace(flow.at("target_intervals")[0].get<SourceSpanId>(), flow.at("source_intervals")[0].get<SourceSpanId>());
    }
    for (const auto & [span, operation] : graph_.operations()) {
        retain_span(operation.host_span, CpuBoundaryKind::Function, operation.name);
        auto workers = operation.worker_spans;
        std::ranges::sort(workers, {}, [&](SourceSpanId worker) { return graph_.boundaries()[graph_.spans()[enqueues.at(worker)].begin].observed_time_ns; });
        for (std::size_t index = 0; index < workers.size(); ++index) {
            retain_span(enqueues.at(workers[index]), CpuBoundaryKind::Enqueue, operation.name, index, workers.size());
            retain_span(workers[index], CpuBoundaryKind::Worker, operation.name, index, workers.size());
        }
    }

    for (const auto & record : records) {
        // Journal entry probes are not profiler service coordinates. Only the
        // actual driver body supplies a new physical timing boundary here.
        if (record.value("ignored", false) || record.at("copy_execution") != "synchronous_driver") continue;
        const auto lane = lanes.find({ record.at("rank").get<std::uint32_t>(), record.at("tid").get<std::uint64_t>() });
        if (lane == lanes.end()) continue;
        const auto & copy = record.at("copy_interval");
        retain(lane->second,
               copy.at("begin_ns").get<std::int64_t>(),
               copy.at("end_ns").get<std::int64_t>(),
               CpuBoundaryKind::Transfer,
               ascend_cpu::driver_copy);
    }
    for (const auto id : host_waits_) {
        const auto & span = graph_.spans()[id];
        auto name = records.at(*span.source_api).at("api").get<std::string>();
        if (span.source_interval) {
            const auto & observed = intervals.at(*span.source_interval).at("name").get_ref<const std::string &>();
            if (observed.starts_with("Runtime@")) name = ascend_cpu::runtime_entry(observed);
        }
        // An SDK-internal wait has no independent lightweight timestamp. Its
        // surrounding CPU preparation forms one readiness interval; the actual
        // wait node and device dependency remain in the execution graph.
        const auto & api = records.at(*span.source_api);
        const auto kind = api.at("copy_execution") == "synchronous_driver" ? CpuBoundaryKind::InternalWait : CpuBoundaryKind::Wait;
        retain_span(id, kind, name);
    }
    for (auto & [point, description] : descriptions) graph_.observe_cpu_boundary(point, std::move(description.first));

    // Framework regions name otherwise unnamed, already observed points. They
    // neither override physical names nor introduce another service cut.
    for (const auto & call : calls_) {
        if (!call.marker) continue;
        const auto name = bindings_.cpu_name(call);
        if (!name) continue;
        const auto & marker = graph_.spans()[*call.marker];
        const auto role = control_lanes.contains(graph_.boundaries()[marker.begin].lane) ? CpuThreadRole::Control : CpuThreadRole::Worker;
        for (const auto & [point, edge] : {
                 std::pair{ marker.begin, CpuBoundaryEdge::Begin },
                 std::pair{   marker.end,   CpuBoundaryEdge::End }
        })
            if (!graph_.cpu_boundaries().contains(point)) graph_.observe_cpu_boundary(point, { role, CpuBoundaryKind::Function, edge, *name });
    }
}

void AscendSourceExtractor::retain_cpu_window(SourceSpanId identity) {
    const auto & span = graph_.spans()[identity];
    const auto & begin = graph_.boundaries()[span.begin];
    cpu_service_boundaries_[begin.lane].insert(begin.observed_time_ns);
    cpu_service_boundaries_[begin.lane].insert(graph_.boundaries()[span.end].observed_time_ns);
}

void AscendSourceExtractor::read_cpu_service_boundaries() {
    // Native scope ends may differ from logical host completion. Retain both:
    // adapters bind ownership, while these observable points anchor sampling.
    for (const auto & call : calls_)
        if (call.marker && (!call.is_annotation() || bindings_.cpu_work_boundary(call))) retain_cpu_window(*call.marker);

    // Operation windows delimit replaceable CPU work. Nested helper returns
    // outside these interfaces do not define additional service units.
    for (const auto & [span, operation] : graph_.operations()) {
        retain_cpu_window(operation.host_span);
        for (const auto identity : operation.worker_spans) retain_cpu_window(identity);
    }

    // Keep every actual CPU-facing dependency, including native API facts that
    // have no vendor flow. Mere annotation extents do not partition service.
    for (const auto & edge : graph_.edges()) {
        if (edge.relation == SourceRelation::IntervalExtent || edge.relation == SourceRelation::ThreadOrder) continue;
        for (const auto point : { edge.source, edge.target }) {
            const auto & boundary = graph_.boundaries()[point];
            if (graph_.lanes()[boundary.lane].domain == SourceDomain::CpuThread) cpu_service_boundaries_[boundary.lane].insert(boundary.observed_time_ns);
        }
    }
}

void AscendSourceExtractor::observe_cpu_control(std::size_t lane, std::int64_t begin_ns, std::int64_t end_ns, std::size_t owner,
                                                std::optional<std::size_t> source_interval, std::optional<std::size_t> recorded_scope) {
    std::vector<std::pair<std::int64_t, std::int64_t>> excluded;
    for (const auto identity : host_waits_) {
        const auto & wait = graph_.spans()[identity];
        const auto & begin = graph_.boundaries()[wait.begin];
        const auto & end = graph_.boundaries()[wait.end];
        if (begin.lane != lane || begin.observed_time_ns >= end_ns || end.observed_time_ns <= begin_ns) continue;
        excluded.emplace_back(std::max(begin_ns, begin.observed_time_ns), std::min(end_ns, end.observed_time_ns));
    }
    // Only real waits split business work. Observer intervals remain source
    // provenance, not extra CPU fragments or template variants. These wall-clock
    // coordinates never price service; independent sampling excludes observer CPU.
    std::sort(excluded.begin(), excluded.end());
    std::vector<std::pair<std::int64_t, std::int64_t>> segments;
    auto cursor_ns = begin_ns;
    for (const auto & [excluded_begin, excluded_end] : excluded) {
        if (cursor_ns < excluded_begin) segments.emplace_back(cursor_ns, excluded_begin);
        cursor_ns = std::max(cursor_ns, excluded_end);
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
            graph_.observe(SourceSpanKind::CpuControl, lane, cursor_ns, next_ns, owner, source_interval, std::nullopt, scope);
            cursor_ns = next_ns;
            if (point != cuts.end()) ++point;
        }
    }
}

void AscendSourceExtractor::extract_cpu_control() {
    const ServingContract contract;
    for (std::size_t index = 0; index < calls_.size(); ++index) {
        const auto & call = calls_[index];
        if (call.is_annotation() || !call.marker) continue;

        // Copies survive observe() growing the graph's span storage.
        const SourceSpan marker = graph_.spans()[*call.marker];
        const auto lane = graph_.boundaries()[marker.begin].lane;
        const auto begin_ns = graph_.boundaries()[marker.begin].observed_time_ns;
        const auto ports = bindings_.ports(graph_, calls_, index);
        const auto completion = ports.find(contract.block(call.step).cpu_completion);
        // Missing native ports are reported by common block extraction. Do not
        // invent a host range when the framework cannot establish its end.
        if (completion == ports.end()) continue;
        const auto end_ns = graph_.boundaries()[completion->second].observed_time_ns;
        observe_cpu_control(lane, begin_ns, end_ns, index, marker.source_interval);
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
        if ((span.kind != SourceSpanKind::CpuObservation && span.kind != SourceSpanKind::ResourceObservation) || !span.owner_call || observer_work(identity))
            continue;
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
                // The first interval is a provenance anchor, not a function-level
                // service identity. Real submission, wait and scope cuts are kept
                // by observe_cpu_control after this coverage union is formed.
                if (coverage && coverage->end_ns == previous_ns && coverage->owner == owner && coverage->scope == scope) { coverage->end_ns = time_ns; }
                else {
                    if (coverage) observe_cpu_control(lane, coverage->begin_ns, coverage->end_ns, coverage->owner, coverage->source_interval, coverage->scope);
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
        if (coverage) observe_cpu_control(lane, coverage->begin_ns, coverage->end_ns, coverage->owner, coverage->source_interval, coverage->scope);

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

} // namespace markov
