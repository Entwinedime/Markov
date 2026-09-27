#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calibration.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

using namespace hicache_timing_fixture;
namespace markov::trace_graph::modules::hicache::runtime {
void check_write_evidence_priority(HiCacheWriteCalls & calls) {
    calls.templates_.clear();
    calls.page_bytes_ = 2;
    const auto add = [&](size_t id, bool independent, uint64_t bytes) {
        HiCacheWriteCalls::WriteTemplate sample;
        sample.pid = sample.tid = "worker";
        sample.write_back = calls.write_back_;
        sample.independent = independent;
        sample.expansion.payload_bytes = bytes;
        sample.expansion.nodes.resize(1);
        sample.expansion.nodes[0].work.name = "AscendCL@aclrtMemcpy2dAsync";
        for (size_t i = 0; i < bytes; ++i) sample.expansion.payload[i] = 1;
        calls.templates_.emplace(id, std::move(sample));
    };
    const auto chosen = [&](size_t id) {
        if (&calls.select_write_template("worker", "worker", 2) != &calls.templates_.at(id))
            throw std::runtime_error("compatible base evidence must precede closer independent write costs");
    };
    add(1, true, 2);
    add(2, false, 4);
    chosen(2); // Independent exact-size sample appears first; base can shrink.
    add(3, false, 2);
    chosen(3); // Within base, retain nearest-size selection.
    calls.templates_.at(3).write_back = !calls.write_back_;
    chosen(2); // Base priority never crosses a policy boundary.
    calls.templates_.at(2).expansion.payload.clear();
    chosen(1); // Incompatible base geometry permits independent fallback.
    calls.templates_.clear();
}
} // namespace markov::trace_graph::modules::hicache::runtime
namespace {
void ordinary_release_export_does_not_require_writeback() {
    core::DagGraph graph;
    for (const auto at : { 100, 110, 120 }) {
        const auto node = graph.add_synthetic_node({
            .name = "release CPU work",
            .lane_key = "main",
            .duration = 10,
            .observed_point = core::DagObservedPoint{ "worker", "worker", static_cast<uint64_t>(at), 0 }
        });
        if (node) graph.add_edge(node - 1, node, core::DagEdgeKind::Sequential);
    }

    core::TraceEvent fact;
    fact.source_channel = core::TraceSourceChannel::PythonProbe;
    fact.name = "hicache_capacity_result_observed_end";
    fact.pid = fact.tid = "worker";
    fact.ts = 100;
    fact.dur = 30;
    fact.set_arg("phase", "end");
    fact.set_arg("fact", R"({"class":"source_actual","role":"capacity_result_observed","consumers":["hicache_dag_patch"]})");
    graph.set_hicache_fact_events({ fact });
    auto release = fact;
    release.name = "runtime.hicache.device_release_regular";
    release.ts = 110;
    release.dur = 10;
    release.set_arg("status", "returned");
    release.set_arg("released_tokens", "128");
    graph.set_runtime_observations({ release });

    const auto output = runtime::export_write_calibration(graph, 100, 130, true);
    require(output.at("scope") == "ordinary release resource evidence" && output.at("rows").size() == 1,
            "ordinary-release export does not depend on a write operation");
    const auto & row = output.at("rows").front();
    require(row.at("released_tokens") == 128 && row.at("rank") == 0 && row.at("template").at("event_waits").empty(),
            "release keeps measured work and resource identity without inventing backup waits");
    graph.set_runtime_observations({});
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)runtime::export_write_calibration(graph, 100, 130, true); },
                                                               "missing ordinary release cannot publish an empty successful calibration");
}

core::TraceEvent admission(const std::string & output_tokens) {
    core::TraceEvent event;
    event.index = 1;
    event.name = "hicache_loadback_decision_observed_end";
    event.pid = event.tid = "worker";
    event.ts = 100;
    event.dur = 40;
    event.source_channel = core::TraceSourceChannel::PythonProbe;
    event.set_arg("fact", R"({"class":"source_actual","role":"loadback_decision_observed","consumers":["hicache_dag_patch"]})");
    event.set_arg("phase", "end");
    event.set_arg("request_id", "r");
    event.set_arg("cache_scope", "cache");
    event.set_arg("effective_token_count", output_tokens);
    return event;
}
model::HiCacheReplayFact input(HiCacheFactRole role, const std::string & request, uint64_t at, uint64_t duration = 0) {
    auto fact = request_fact("unused_by_observer", request, at, 0);
    fact.dur = duration;
    return { fact, role };
}
void complete_load_branches_keep_lookup_identity() {
    core::TraceEvent branch;
    branch.name = "hicache.control.host_load_branch";
    branch.pid = branch.tid = "worker";
    branch.ts = 100; branch.ts_submicro_ns = 500; branch.dur = 40;
    auto check = branch;
    check.name = "runtime.hicache.host_load_check";
    check.ts = 110; check.dur = 2;
    check.set_arg("request_id", "r"); check.set_arg("needed", "false"); check.set_arg("status", "returned");
    std::vector<core::TraceEvent> events{branch, check};
    std::vector<model::HiCacheReplayFact> facts{input(HiCacheFactRole::CacheLookupInput, "r", 70),
        input(HiCacheFactRole::CacheLookupInput, "other", 95), input(HiCacheFactRole::CacheLookupInput, "r", 90)};
    auto observed = runtime::observe_load_branches(events, facts, 0, 300);
    require(observed.size() == 1 && observed.front().owner == 90 && observed.front().condition.arg("needed") == "false"
                && observed.front().envelope.ts_submicro_ns == 500,
            "false branch retains exact envelope and latest same-request lookup, not the nearest unrelated input");
    auto later = branch; later.ts = 200;
    auto later_check = check; later_check.ts = 210; later_check.set_arg("needed", "true");
    events.push_back(later); events.push_back(later_check);
    facts.push_back(input(HiCacheFactRole::CacheLookupInput, "r", 190));
    observed = runtime::observe_load_branches(events, facts, 0, 300);
    require(observed.size() == 2 && observed[1].owner == 190, "repeated requests retain their separate lookup occurrences");
    require(runtime::observe_load_branches({}, facts, 0, 300).empty(), "older protocol does not invent complete branches");
    const auto reject = [&](std::vector<core::TraceEvent> wrong, std::vector<model::HiCacheReplayFact> inputs) {
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)runtime::observe_load_branches(wrong, inputs, 0, 300); },
                                                                   "incomplete or ambiguous branch identity cannot become an insertion site");
    };
    reject({branch}, facts);
    reject({check}, facts);
    reject({branch, check, check}, facts);
    reject({branch, check}, {});
    auto wrong = check; wrong.tid = "different-thread"; reject({branch, wrong}, facts);
    wrong = check; wrong.set_arg("status", "raised"); reject({branch, wrong}, facts);
    wrong = check; wrong.set_arg("needed", "unknown"); reject({branch, wrong}, facts);
    wrong = check; wrong.ts = 100; wrong.ts_submicro_ns = 499; reject({branch, wrong}, facts);
    auto duplicate = facts[2]; duplicate.fact.cache_scope = "another-cache";
    facts.push_back(duplicate); reject({branch, check}, facts);
}

void controller_identity_does_not_require_a_nonempty_load() {
    core::DagGraph graph;
    auto tree = admission("0");
    tree.set_arg("fact", R"({"class":"source_actual","role":"commit_device_to_host_enqueue_observed","consumers":["hicache_dag_patch"]})");
    tree.set_arg("cache_scope", "tree");
    auto controller = tree;
    controller.index = 2; controller.ts = 110; controller.dur = 10;
    controller.set_arg("fact", R"({"class":"timing_observation","role":"commit_device_to_host_io_observed","consumers":["hicache_dag_patch"]})");
    controller.set_arg("cache_scope", "controller");
    const auto observe = [&] { return runtime::observe_write_controller_scopes(patch::HiCacheSourceDagIndex(graph)); };
    graph.set_hicache_fact_events({tree, controller});
    require(observe().at({"worker", "controller"}) == "tree", "nested calls establish identity without loads or CPU templates");
    graph.set_hicache_fact_events({});
    graph.set_prelude_context_events({tree, controller});
    require(observe().at({"worker", "controller"}) == "tree" && graph.nodes().empty(),
            "pre-window calls prove persistent identity without adding any executable work or cost");
    graph.set_prelude_context_events({});
    controller.tid = "another thread";
    graph.set_hicache_fact_events({tree, controller});
    require(observe().empty(), "overlap on another thread does not prove ownership");
    controller.tid = "worker"; controller.ts = 200;
    graph.set_hicache_fact_events({tree, controller});
    require(observe().empty(), "nearest tree call is not ownership evidence");
    controller.ts = 110;
    auto other = tree; other.index = 3; other.set_arg("cache_scope", "other tree");
    graph.set_hicache_fact_events({tree, other, controller});
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)observe(); }, "conflicting resource identities must not silently choose a tree");
}
void host_template_keeps_waits_and_workers_separate() {
    std::vector<core::TraceEvent> events(6);
    const std::vector<uint64_t> starts{ 100, 120, 130, 150, 200, 105 };
    const std::vector<uint64_t> durations{ 10, 5, 10, 1, 7, 2 };
    for (size_t i = 0; i < events.size(); ++i) {
        events[i].name = "host leaf";
        events[i].pid = "worker";
        events[i].tid = i < 4 ? "worker" : "runtime";
        events[i].ts = starts[i];
        events[i].dur = durations[i];
    }
    core::DagGraph graph(std::move(events), 0);
    for (size_t i = 0; i < 6; ++i) graph.add_node(i, true, i < 4 ? "main" : "runtime");
    graph.add_edge(0, 4, core::DagEdgeKind::Correlation);
    const auto device = graph.add_synthetic_node({ .name = "earlier compute", .is_cpu = false });
    const auto wait = graph.add_edge(device, 1, core::DagEdgeKind::Sync);
    auto outer = admission("0");
    outer.name = "hicache_commit_device_to_host_enqueue_observed_end";
    outer.set_arg("fact", R"({"class":"source_actual","role":"commit_device_to_host_enqueue_observed","consumers":["hicache_dag_patch"]})");
    graph.set_hicache_fact_events({ outer });
    patch::HiCacheIoOperationRecord record;
    record.kind = patch::HiCacheIoOperationKind::WriteDeviceToHost;
    record.pid = record.tid = "worker";
    record.source_start_us = 115;
    record.source_end_us = 135;
    const auto observed = runtime::observe_write_host_template(patch::HiCacheSourceDagIndex(graph), record);
    require(observed.main.owned_node_duration_us == 25 && observed.main.owned_gap_duration_us == 15,
            "host template separates executable leaves from residual gaps in the outer call");
    require(observed.worker_nodes == std::vector<size_t>{ 4 },
            "worker ownership follows submission, including late work but excluding unrelated overlapping tasks");
    require(observed.device_wait_edges == std::vector<size_t>{ wait }, "native device waiting remains an explicit dependency, not aggregate CPU service");
    require(!observed.write_back.has_value(), "legacy template without policy is not known write-through");
    for (const bool write_back : {false, true}) {
        outer.set_arg("write_back", write_back ? "true" : "false");
        graph.set_hicache_fact_events({outer});
        const auto typed = runtime::observe_write_host_template(patch::HiCacheSourceDagIndex(graph), record);
        require(typed.write_back == std::optional<bool>{write_back}, "host template retains the actual outer write policy");
        require(typed.main.owned_node_duration_us == observed.main.owned_node_duration_us,
                "policy classification does not invent or remove CPU costs");
    }
    graph.mutable_event_for_node(0).ts = 99;
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)runtime::observe_write_host_template(patch::HiCacheSourceDagIndex(graph), record); },
                                                               "a call cutting through an executable leaf needs exact partitioning before template extraction");
    const patch::HiCacheSourceDagIndex structural_source(graph);
    const auto & envelope = runtime::observe_write_envelope(
        structural_source, record.pid, record.tid, record.source_start_us, record.source_end_us);
    require(envelope.timestamp_us == outer.ts && envelope.duration_us == outer.dur,
            "source call ownership remains available when its CPU cost template cannot be extracted");
}

void eviction_regions_keep_outer_calls_and_release_work() {
    core::DagGraph graph;
    const auto leaf = [&](uint64_t at, uint64_t duration, const char * tid = "worker") {
        return graph.add_synthetic_node({
            .name = "CPU leaf",
            .lane_key = tid,
            .duration = duration,
            .observed_point = core::DagObservedPoint{ "worker", tid, at, 0 }
        });
    };
    leaf(100, 10);
    leaf(110, 40);
    leaf(150, 10);
    const auto wait_leaf = leaf(160, 20);
    const auto release = leaf(180, 20);
    leaf(200, 1);
    const auto late_worker = leaf(210, 7, "runtime");
    leaf(185, 2, "runtime"); // Unrelated overlapping worker must not be copied.
    graph.add_edge(release, late_worker, core::DagEdgeKind::Correlation);
    const auto device = graph.add_synthetic_node({ .name = "write complete", .is_cpu = false, .lane_key = "device" });
    const auto wait_edge = graph.add_edge(device, wait_leaf, core::DagEdgeKind::Sync);
    auto outer = admission("0");
    outer.ts = 110;
    outer.dur = 40;
    outer.set_arg("fact", R"({"class":"source_actual","role":"commit_device_to_host_enqueue_observed","consumers":["hicache_dag_patch"]})");
    auto inner = outer;
    inner.index = 2;
    inner.ts = 120;
    inner.dur = 10;
    inner.set_arg("fact", R"({"class":"source_actual","role":"commit_device_to_host_io_observed","consumers":["hicache_dag_patch"]})");
    graph.set_hicache_fact_events({ outer, inner });
    core::TraceEvent check;
    check.name = "runtime.hicache.write_completion";
    check.pid = check.tid = "worker";
    check.ts = 160;
    check.dur = 20;
    check.set_arg("blocking", "true");
    graph.set_runtime_observations({ check });
    patch::HiCacheSourceFactNode evict;
    evict.fact_role = "capacity_result_observed";
    evict.phase = "end";
    evict.pid = evict.tid = "worker";
    evict.timestamp_us = 100;
    evict.duration_us = 100;
    using Kind = runtime::HiCacheEvictionRegion::Kind;
    const patch::HiCacheSourceDagIndex source(graph);
    const auto regions = runtime::observe_eviction_regions(source, evict);
    using Phase = runtime::HiCacheEvictionRegion::ControlPhase;
    require(regions[0].control_phase == Phase::Setup && regions[2].control_phase == Phase::SelectionEnd
                && regions[4].control_phase == Phase::Finish,
            "initialization, final selection bookkeeping and tail work are distinct donors");
    require(regions.size() == 5 && regions[1].kind == Kind::WriteBackup && regions[1].begin == 110 && regions[1].end == 150,
            "eviction excludes the full outer write, not just the nested I/O call");
    require(regions[2].preceding_kind == Kind::WriteBackup,
            "selection tail context belongs to the full observed write call");
    uint64_t covered = 0;
    for (const auto & region : regions) covered += region.end - region.begin;
    require(covered == 100 && regions[3].kind == Kind::BlockingCheck && regions[4].kind == Kind::Control,
            "source eviction regions partition the wall clock without dropping post-wait release");
    const auto host = runtime::observe_host_template(source, "worker", "worker", 180, 200);
    require(host.main.owned_node_ids == std::vector<size_t>{ release } && host.worker_nodes == std::vector<size_t>{ late_worker },
            "release template owns late submitted work, not unrelated overlap");
    require(runtime::observe_host_template(source, "worker", "worker", 160, 180).device_wait_edges == std::vector<size_t>{ wait_edge },
            "blocking check retains device completion dependency separately from CPU service");
    graph.set_hicache_fact_events({});
    const auto no_write = runtime::observe_eviction_regions(patch::HiCacheSourceDagIndex(graph), evict);
    require(no_write.size() == 3 && no_write[0].end == 160 && no_write[2].begin == 180, "eviction without writes retains control on both sides of its check");
    require(no_write[0].control_phase == Phase::Empty, "an empty loop is not a measured initialization or per-victim sample");
    auto release_call = check;
    release_call.name = "runtime.hicache.device_release_backup";
    release_call.ts = 180;
    release_call.dur = 20;
    release_call.set_arg("status", "returned");
    release_call.set_arg("released_tokens", "128");
    graph.set_runtime_observations({ check, release_call });
    const auto separated = runtime::observe_eviction_regions(patch::HiCacheSourceDagIndex(graph), evict);
    require(separated.size() == 3 && separated[2].kind == Kind::ReleaseBackup && separated[2].released_tokens == 128 && separated[0].begin == 100
                && separated[0].end == 160,
            "per-victim release is removed from residual control and keeps its observed geometry");
    release_call.name = "runtime.hicache.device_release_regular";
    release_call.set_arg("released_tokens", "0");
    graph.set_runtime_observations({ check, release_call });
    const auto regular = runtime::observe_eviction_regions(patch::HiCacheSourceDagIndex(graph), evict);
    require(regular[2].kind == Kind::ReleaseRegular && regular[2].released_tokens == 0, "regular release and zero scalar return remain explicit observations");
    auto first_release = release_call;
    first_release.ts = 110;
    first_release.dur = 5;
    auto second_release = first_release;
    second_release.ts = 125;
    auto after_check = first_release;
    after_check.ts = 183;
    auto last_release = first_release;
    last_release.ts = 190;
    graph.set_runtime_observations({ first_release, second_release, check, after_check, last_release });
    const auto loops = runtime::observe_eviction_regions(patch::HiCacheSourceDagIndex(graph), evict);
    require(!loops.front().preceding_kind && loops[2].preceding_kind == Kind::ReleaseRegular
                && loops[4].preceding_kind == Kind::ReleaseRegular,
            "selection controls retain the observed preceding operation rather than a phase-only label");
    std::vector<Phase> phases;
    uint64_t loop_coverage = 0;
    for (const auto & region : loops) {
        loop_coverage += region.end - region.begin;
        if (region.kind == Kind::Control) phases.push_back(region.control_phase);
        else require(region.control_phase == Phase::None, "actual calls must not acquire residual-control ownership");
    }
    require(phases == std::vector<Phase>{ Phase::Setup, Phase::Selection, Phase::SelectionEnd, Phase::ReleaseStart, Phase::ReleaseNext, Phase::Finish }
                && loop_coverage == 100,
            "both loops retain distinct iteration work without changing wall-clock coverage");
    graph.set_runtime_observations({ first_release, second_release });
    const auto without_check = runtime::observe_eviction_regions(patch::HiCacheSourceDagIndex(graph), evict);
    require(without_check.size() == 5 && without_check[0].control_phase == Phase::Setup
                && without_check[2].control_phase == Phase::Selection && without_check[4].control_phase == Phase::Finish,
            "nonblocking-policy eviction has one selection loop and no invented confirmation or post-write loop");
    graph.set_runtime_observations({ check });
    outer.ts = 150;
    graph.set_hicache_fact_events({ outer });
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)runtime::observe_eviction_regions(patch::HiCacheSourceDagIndex(graph), evict); },
                                                               "overlapping outer calls cannot produce double-owned control work");
}

void first_cpu_entry() {
    core::TraceEvent first, after;
    first.name = "write wait CPU";
    first.pid = first.tid = "worker";
    first.ts = 120;
    first.dur = 10;
    after = first;
    after.name = "after";
    after.ts = 150;
    after.dur = 1;
    auto graph = core::DagBuilder(1).build({ first, after }, 0);
    auto identity = admission("0");
    identity.name = "hicache_cache_lifecycle_commit_end";
    identity.ts = 100;
    identity.dur = 100;
    identity.set_arg("fact", R"({"class":"workload_identity","role":"cache_lifecycle_commit","consumers":["hicache_state_model"]})");
    identity.set_arg("seq_no", "1");
    identity.set_arg("lifecycle_kind", "finished");
    identity.set_arg("token_count", "4");
    identity.set_arg("full_path_span", R"({"path_id":"p","begin":0,"end":4,"token_count":4})");
    identity.set_arg("token_dictionary", R"({"path_id":"p","token_ids":[1,2,3,4]})");
    graph.set_hicache_fact_events({ identity });
    auto wait = first;
    wait.name = "runtime.hicache.write_completion";
    wait.set_arg("blocking", "true");
    wait.set_arg("status", "returned");
    graph.set_runtime_observations({ wait });
    auto point = first;
    point.dur = 0;
    require(!core::insert_cpu_gap_points(graph, std::span(&point, 1))[0], "first CPU entry has no preceding gap to split");
    model::HiCacheModelReplay replay(graph, timing_config(), true);
    require(replay.facts().size() == 1, "binding fixture has one validated lifecycle identity");
    runtime::HiCacheWriteStreamInsertions insertions;
    runtime::HiCacheWriteCalls calls(replay, timing_config(), insertions);
    calls.bind(graph, 100, 200);
    runtime::check_write_evidence_priority(calls);
}

void inactive_load_tail_is_replaced_once(bool target_load = false, bool needs_eviction = false) {
    std::vector<core::TraceEvent> events(3);
    for (auto & event : events) event.pid = event.tid = "worker";
    events[0].name = "before"; events[0].ts = 100; events[0].dur = 10;
    events[1].name = "hicache.control.host_load_branch.self"; events[1].ts = 120; events[1].dur = 40;
    events[2].name = "after"; events[2].ts = 170; events[2].dur = 1;
    if (target_load) {
        auto donor = events[1]; donor.ts = 220; donor.dur = 100;
        donor.set_arg("hicache_control_parent", "hicache.control.host_load_branch");
        donor.set_arg("hicache_control_parent_index", "42");
        donor.set_arg("hicache_control_semantics", "parent_self_time");
        events.push_back(donor);
        auto after = events[2]; after.ts = 330; events.push_back(after);
    }
    auto graph = core::DagBuilder(1).build(events, 0);
    (void)simulation::run_topological_simulation(graph);
    const auto expected = graph.node(2).completion_time;
    auto identity = admission("0");
    identity.name = "hicache_cache_lookup_input_end";
    identity.ts = 100; identity.dur = 0;
    identity.set_arg("fact", R"({"class":"workload_identity","role":"cache_lookup_input","consumers":["hicache_state_model"]})");
    identity.set_arg("seq_no", "1"); identity.set_arg("token_count", "4");
    identity.set_arg("full_path_span", R"({"path_id":"p","begin":0,"end":4,"token_count":4})");
    identity.set_arg("token_dictionary", R"({"path_id":"p","token_ids":[1,2,3,4]})");
    graph.set_hicache_fact_events({identity});
    auto branch = events[1]; branch.name = "hicache.control.host_load_branch"; branch.ts = 110; branch.dur = 50;
    auto condition = branch; condition.name = "runtime.hicache.host_load_check"; condition.dur = 5;
    condition.set_arg("request_id", "r"); condition.set_arg("status", "returned"); condition.set_arg("needed", "false");
    graph.set_runtime_observations({branch, condition});
    auto config = timing_config(); config.l1_capacity_pages = 9; config.l2_capacity_pages = 24;
    if (needs_eviction) config.l1_capacity_pages = 8;
    if (target_load) {
        // Seeding two-page entries into nine pages leaves one free page.
        // This variant exercises direct admission, not eviction primitives.
        identity.set_arg("token_count", "16");
        identity.set_arg("full_path_span", R"({"path_id":"p","begin":0,"end":16,"token_count":16})");
        identity.set_arg("token_dictionary", R"({"path_id":"p","token_ids":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]})");
        auto donor_input = identity; donor_input.index = 2; donor_input.ts = 210; donor_input.set_arg("request_id", "donor");
        auto donor_admission = admission("16"); donor_admission.index = 3; donor_admission.ts = 250; donor_admission.dur = 50;
        donor_admission.set_arg("request_id", "donor");
        auto donor_branch = branch; donor_branch.ts = 220; donor_branch.dur = 100;
        auto donor_condition = condition; donor_condition.ts = 220; donor_condition.dur = 10;
        donor_condition.set_arg("request_id", "donor"); donor_condition.set_arg("needed", "true");
        auto allocation = donor_condition; allocation.name = "runtime.hicache.load_allocation";
        allocation.ts = 260; allocation.dur = 20; allocation.set_arg("allocated", "true"); allocation.set_arg("node_id", "7");
        std::vector<core::TraceEvent> observations{branch, condition, donor_branch, donor_condition, allocation};
        std::vector<core::TraceEvent> facts{identity, donor_input, donor_admission};
        if (needs_eviction) {
            facts[2].ts = 235; facts[2].dur = 65;
            auto capacity = donor_admission; capacity.index = 4; capacity.ts = 245; capacity.dur = 10;
            capacity.set_arg("fact", R"({"class":"source_actual","role":"capacity_result_observed","consumers":["hicache_dag_patch"]})");
            facts.push_back(capacity);
            auto failed = allocation; failed.ts = 240; failed.dur = 3; failed.set_arg("allocated", "false");
            observations.push_back(failed);
            auto release = allocation; release.name = "runtime.hicache.device_release_backup"; release.ts = 248; release.dur = 4;
            release.set_arg("released_tokens", "32"); observations.push_back(release);
        }
        graph.set_hicache_fact_events(std::move(facts));
        graph.set_runtime_observations(std::move(observations));
    }
    model::HiCacheModelReplay replay(graph, config, true);
    require(replay.facts().size() == (target_load ? 2 : 1), "branch fixture has valid lookup inputs");
    runtime::HiCacheWriteStreamInsertions insertions;
    runtime::HiCacheWriteCalls calls(replay, config, insertions);
    calls.bind(graph, 100, 400);
    if (target_load) {
        replay.state().begin_formal_window(false);
        seed_storage(replay.state()); replay.state().begin_formal_window(true);
        const auto candidate = request_fact("prefetch_candidate_anchor", "r", 200'099, 0);
        replay.state().apply_fact(candidate, HiCacheFactRole::PrefetchCandidateAnchor);
    }
    replay.apply(replay.facts().front().fact.source_node_id, 200'100);
    require(bool(replay.state().load_admission_work(replay.facts().front().fact)) == target_load, "target state, not source condition, chooses the branch");
    require(bool(replay.state().eviction_work(replay.facts().front().fact)) == needs_eviction,
            "the capacity variant must actually select target eviction work");
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t elapsed, simulation::FutureDag & future) {
        (void)calls.advance(node, 200'100 + elapsed, future);
    });
    require(graph.node(2).completion_time == expected + (target_load ? 45 : 0),
            "new 30+20+40us admission replaces the 45us false tail; neither path runs twice or releases its continuation early");

    hicache_timing_fixture::require_static_replay(graph, "restored branch survives static replay");
}
core::DagGraph self_graph() {
    std::vector<core::TraceEvent> events(3);
    for (auto & event : events) event.pid = event.tid = "worker";
    events[0].name = "before";
    events[0].ts = 100;
    events[0].dur = 10;
    events[1].name = "hicache.control.load_back_admission.self";
    events[1].ts = 120;
    events[1].dur = 40;
    events[1].set_arg("hicache_control_parent", "hicache.control.load_back_admission");
    events[1].set_arg("hicache_control_parent_index", "42");
    events[1].set_arg("hicache_control_semantics", "parent_self_time");
    events[2].name = "after";
    events[2].ts = 170;
    events[2].dur = 5;
    core::DagGraph graph(std::move(events), 3);
    for (size_t i = 0; i < 3; ++i) graph.add_node(i, true, "main");
    for (size_t i = 0; i < 2; ++i) {
        graph.mutable_node(i).cpu_gap_after = graph.mutable_node(i).original_cpu_gap_after = 10;
        graph.add_edge(i, i + 1, core::DagEdgeKind::Sequential);
    }
    const auto consumer = graph.add_synthetic_node({ .name = "device consumer", .is_cpu = false, .duration = 5, .counts_toward_e2e = true });
    graph.add_edge(1, consumer, core::DagEdgeKind::Sync);
    return graph;
}
void partition_self_time() {
    auto graph = self_graph();
    const auto baseline = simulation::run_topological_simulation(graph);
    const auto original = graph.nodes();
    std::vector<core::TraceEvent> points(3);
    for (auto & point : points) {
        point.name = "write submission";
        point.pid = point.tid = "worker";
    }
    points[0].ts = 145;
    points[1].ts = points[2].ts = 130;
    {
        auto measured = self_graph();
        measured.cpu_service_cost().add({"worker", "worker"}, {120, 160, 20});
        const auto baseline = simulation::run_topological_simulation(measured);
        const auto original = measured.nodes();
        const auto boundaries = bind_hicache_control_points(measured, points);
        require(boundaries[0] && boundaries[1], "measured control self-time retains observable boundaries");
        require(simulation::run_topological_simulation(measured).e2e_us == baseline.e2e_us,
                "control self-time partition must preserve measured CPU service");
        for (const auto & node : original)
            require(measured.node(node.id).completion_time == node.completion_time,
                    "measured control partition preserves original consumers");
    }
    const auto bound = bind_hicache_control_points(graph, points);
    require(bound[0] && bound[1] && bound[1] == bound[2], "self-time partitions sort and deduplicate observed instants");
    require(simulation::run_topological_simulation(graph).e2e_us == baseline.e2e_us, "partitioning self-time cannot change the business endpoint");
    for (const auto & node : original)
        require(graph.node(node.id).completion_time == node.completion_time, "all existing completion dependencies retain their exact timing");
    require(graph.node(*bound[0]).completion_time == 45 && graph.node(*bound[1]).completion_time == 30,
            "interior points execute at the measured CPU offsets, not the enclosing leaf entry or return");
    require(graph.node(1).duration == 15 && graph.node(1).cpu_gap_after == 10, "original node remains the final work fragment and keeps its tail gap");
    const auto count = graph.node_count();
    require(bind_hicache_control_points(graph, points) == bound && graph.node_count() == count, "binding the same points again adds no work or boundaries");
    const patch::HiCacheSourceDagIndex source(graph);
    const auto ownership = source.enclosing_control_interval_ownership({ .timestamp_us = 133, .duration_us = 1, .pid = "worker", .tid = "worker" },
                                                                       "hicache.control.load_back_admission");
    require(ownership && ownership->interval_start_us == 120 && ownership->interval_end_us == 160,
            "split leaves preserve their full control-envelope identity");
    graph.set_node_duration(0, 17);
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(*bound[1]).completion_time == 37 && graph.node(2).completion_time == 82,
            "bound points and downstream work move with modeled CPU execution rather than staying fixed on source time");
    const auto reject = [&](core::DagGraph candidate) {
        require(!bind_hicache_control_points(candidate, std::span(points).first(1))[0], "only unchanged unowned control self-time may be partitioned");
    };
    auto native = self_graph();
    native.mutable_event_for_node(1).set_arg("hicache_control_semantics", "");
    reject(std::move(native));
    auto changed = self_graph();
    changed.set_node_duration(1, 41);
    reject(std::move(changed));
    auto task = self_graph();
    task.mutable_node(1).explicit_cpu_task = true;
    reject(std::move(task));
    auto owned = self_graph();
    owned.set_scope_node_owned(1);
    reject(std::move(owned));
}
} // namespace

void check_write_call_binding() {
    ordinary_release_export_does_not_require_writeback();
    controller_identity_does_not_require_a_nonempty_load();
    inactive_load_tail_is_replaced_once();
    inactive_load_tail_is_replaced_once(true);
    inactive_load_tail_is_replaced_once(true, true);
    complete_load_branches_keep_lookup_identity();
    host_template_keeps_waits_and_workers_separate();
    eviction_regions_keep_outer_calls_and_release_work();
    first_cpu_entry();
    partition_self_time();
    std::vector<model::HiCacheReplayFact> facts{
        input(HiCacheFactRole::CacheLookupInput, "r", 20),
        input(HiCacheFactRole::PrefetchCandidateAnchor, "r", 30),
        input(HiCacheFactRole::CacheLookupInput, "r", 60),
        input(HiCacheFactRole::CacheLookupInput, "unrelated", 75),
        input(HiCacheFactRole::CacheLifecycleCommit, "finished", 200, 50),
        input(HiCacheFactRole::CacheDecodeAllocation, "decode", 250, 30),
        input(HiCacheFactRole::CacheExtendInput, "extend", 300),
        input(HiCacheFactRole::CacheLifecycleCommit, "extend", 400, 10),
    };
    for (const std::string output : { "0", "512" }) {
        core::DagGraph graph;
        graph.set_hicache_fact_events({ admission(output) });
        const patch::HiCacheSourceDagIndex source(graph);
        const auto owner = [&](uint64_t begin, uint64_t end, const std::string & pid = "worker", const std::string & tid = "worker") {
            return runtime::observe_write_call_owner(source, facts, pid, tid, begin, end);
        };
        require(owner(110, 120) == 60, "explicit loadback identity chooses the admitted request, not a nearer unrelated lookup");
        require(owner(125, 138) == 60, "write submission and blocking return share the same admission action");
        require(owner(160, 180) == 200, "lifecycle source_ts is its end; nested writes belong to the enclosing entry/return envelope");
        require(owner(255, 275) == 250, "derived decode source_ts is its entry, not its return");
        require(owner(320, 350) == 300, "extend owns the same-thread region before the next state action");
        const auto reject = [&](uint64_t begin, uint64_t end, const std::string & pid = "worker", const std::string & tid = "worker") {
            hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)owner(begin, end, pid, tid); },
                                                                       "a write outside the same-thread allocation region must remain unbound");
        };
        reject(320, 410);
        reject(110, 120, "other");
        reject(110, 120, "worker", "other");
        auto invalid = facts;
        invalid[0].fact.cache_scope = invalid[2].fact.cache_scope = "other-cache";
        hicache_timing_fixture::require_throws<std::runtime_error>(
            [&] { (void)runtime::observe_write_call_owner(source, invalid, "worker", "worker", 110, 120); },
            "a matching request ID in another cache is not the owner");
    }
    core::DagGraph guard_graph;
    const patch::HiCacheSourceDagIndex guard_source(guard_graph);
    core::TraceEvent guard;
    guard.name = "runtime.hicache.capacity_guard";
    guard.pid = guard.tid = "worker";
    guard.ts = 320;
    guard.dur = 20;
    guard.set_arg("status", "returned");
    std::vector<core::TraceEvent> observations{ guard };
    auto guards = runtime::observe_capacity_guards(guard_source, facts, observations, 0, 500);
    require(guards.size() == 1 && guards[0].owner == 300 && guards[0].begin == 320 && guards[0].end == 340,
            "an allocation without any source eviction still exposes its exact capacity interval");
    observations[0].ts = 255;
    guards = runtime::observe_capacity_guards(guard_source, facts, observations, 0, 500);
    require(guards.size() == 1 && guards[0].owner == 250, "decode capacity belongs to its enclosing allocation");
    require(runtime::observe_capacity_guards(guard_source, facts, observations, 280, 500).empty(), "out-of-window guards are not bound");
    const auto reject_guard = [&] {
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)runtime::observe_capacity_guards(guard_source, facts, observations, 0, 500); },
                                                                   "ambiguous or incomplete capacity observations must not become target insertion sites");
    };
    observations.push_back(observations[0]);
    reject_guard();
    observations.resize(1);
    observations[0].set_arg("status", "raised");
    reject_guard();
    observations[0].set_arg("status", "returned");
    observations[0].ts = 160;
    reject_guard();
}
