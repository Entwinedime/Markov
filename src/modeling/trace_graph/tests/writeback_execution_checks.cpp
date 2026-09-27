#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/modules/hicache/runtime/eviction_sequence.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

using namespace hicache_timing_fixture;
namespace {
HiCacheFact path(std::string role, std::string request, uint64_t at, uint32_t first, uint64_t tokens) {
    auto fact = request_fact(std::move(role), std::move(request), at, first);
    fact.token_count = fact.full_path_span.end = fact.full_path_span.token_count = tokens;
    fact.full_path_tokens.resize(tokens);
    fact.lifecycle_kind = "finished";
    return fact;
}
void lifecycle_cursor_waits_for_whole_target_nodes() {
    for (const uint64_t page_size : {8, 16}) {
        auto config = timing_config();
        config.page_size = config.kv_bytes_per_page = page_size;
        config.l1_capacity_pages = 8;
        model::HiCacheState state(config);
        state.begin_formal_window(true);
        auto fact = path("cache_lifecycle_commit", "stepped", 10, 0, 31);
        state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit, true, model::HiCacheLifecycleExecution::Stepped);
        require(state.lifecycle_insert_pending(fact) && state.pending_device_writes(fact).empty(),
                "entry does not materialize or write the future path");
        state.advance_lifecycle_insert(fact, 8);
        require(state.pending_device_writes(fact).empty(), "a source prefix cannot split a larger target leaf early");
        hicache_timing_fixture::require_throws<std::logic_error>([&] { state.complete_lifecycle_return(fact); },
                                                                 "request ownership survives incomplete insertion");
        fact.ts = 20;
        state.advance_lifecycle_insert(fact, 16);
        require(state.pending_device_writes(fact).size() == (page_size == 16 ? 1 : 0),
                "target paging, not source node identity, determines when the whole leaf is ready");
        fact.ts = 30;
        state.advance_lifecycle_insert(fact, 31);
        require(!state.lifecycle_insert_pending(fact) && state.lifecycle_return_pending(fact),
                "unaligned input tail finishes insertion without releasing the request");
        const auto writes = state.pending_device_writes(fact);
        require(writes.size() == 1 && writes.front().header.pages.size() == 31 / page_size,
                "only target-aligned pages enter the write");
        state.advance_lifecycle_insert(fact, 31);
        require(state.pending_device_writes(fact).size() == 1, "repeated ready boundary does not resubmit");
        hicache_timing_fixture::require_throws<std::logic_error>([&] { state.advance_lifecycle_insert(fact, 16); }, "ready prefix cannot move backwards");
        fact.ts = 40;
        state.complete_lifecycle_return(fact);
        require(!state.lifecycle_return_pending(fact), "separate return completes the lifecycle");
    }
    auto config = timing_config();
    config.write_policy = "write_through_selective";
    config.write_through_threshold = 2;
    model::HiCacheState state(config);
    state.begin_formal_window(true);
    auto fact = path("cache_lifecycle_commit", "first", 10, 0, 16);
    state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit, true, model::HiCacheLifecycleExecution::Stepped);
    state.advance_lifecycle_insert(fact, 16);
    state.advance_lifecycle_insert(fact, 16);
    require(state.pending_device_writes(fact).empty(), "repeated boundary does not add a second selective hit");
    state.complete_lifecycle_return(fact);
    fact = path("cache_lifecycle_commit", "second", 20, 0, 16);
    state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit);
    require(state.pending_device_writes(fact).size() == 1, "next actual insertion adds the second selective hit");
}

void lifecycle_return_is_separate_from_write_submission() {
    for (const auto * kind : { "finished", "unfinished" }) {
        model::HiCacheState state(timing_config());
        state.begin_formal_window(true);
        auto fact = path("cache_lifecycle_commit", "active", 10, 0, 16);
        fact.lifecycle_kind = kind;
        state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit, true, model::HiCacheLifecycleExecution::DeferReturn);
        require(state.lifecycle_return_pending(fact), "insertion leaves the explicitly deferred return pending");
        const auto writes = state.pending_device_writes(fact);
        require(writes.size() == 1, "write submission exists before request return");
        hicache_timing_fixture::require_throws<std::logic_error>([&] { state.finalize(); }, "finalization cannot hide an unexecuted lifecycle return");
        auto other = fact;
        other.source_node_id += 1;
        bool wrong_owner = false;
        try { state.complete_lifecycle_return(other); }
        catch (const std::logic_error &) { wrong_owner = true; }
        require(wrong_owner && state.lifecycle_return_pending(fact), "wrong return must preserve the pending owner");
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { state.apply_fact(other, HiCacheFactRole::CacheLifecycleCommit); },
                                                                   "new state actions cannot skip a pending same-scope return");
        fact.ts = 20;
        state.complete_lifecycle_return(fact);
        require(!state.lifecycle_return_pending(fact), "return completes once at its own boundary");
        const auto after = state.pending_device_writes(fact);
        require(after.size() == 1 && after.front().header.operation_id == writes.front().header.operation_id,
                "return neither resubmits nor acknowledges asynchronous D2H");
        hicache_timing_fixture::require_throws<std::logic_error>([&] { state.complete_lifecycle_return(fact); },
                                                                 "duplicate return cannot release the request twice");
    }
}

void chunked_insert_does_not_count_as_a_write_hit() {
    auto config = timing_config();
    config.write_policy = "write_through_selective";
    config.write_through_threshold = 2;
    model::HiCacheState state(config);
    state.begin_formal_window(true);
    auto fact = path("cache_lifecycle_commit", "chunked", 1, 0, 16);
    fact.lifecycle_kind = "unfinished";
    fact.chunked.reset();
    bool missing = false;
    try { state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit); }
    catch (const std::runtime_error & error) {
        missing = std::string(error.what()).find("lacks chunked observation") != std::string::npos;
    }
    require(missing, "unknown chunked state cannot silently become an ordinary hit");
    fact.chunked = true;
    state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit);
    require(state.pending_device_writes(fact).empty(), "chunked insertion does not issue a write");
    fact.chunked = false;
    fact.ts = fact.source_ts = 2;
    state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit);
    require(state.pending_device_writes(fact).empty(), "first ordinary visit after chunked insertion is only hit one");
    fact.ts = fact.source_ts = 3;
    state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit);
    require(state.pending_device_writes(fact).size() == 1, "second ordinary visit reaches the selective threshold");
}

void seed(model::HiCacheState & state, uint64_t at, uint32_t first, uint64_t tokens) {
    auto fact = path("cache_lifecycle_commit", "seed" + std::to_string(at), at, first, tokens);
    state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit, false);
    state.finalize();
}
HiCacheFact extend(uint64_t at = 100) {
    auto fact = path("cache_extend_input", "active", at, 2'000, 16);
    fact.is_start = true;
    fact.is_end = false;
    fact.batch_paths = {
        { .request_id = fact.request_id, .full_path_span = fact.full_path_span, .full_path_tokens = fact.full_path_tokens, .token_count = fact.token_count }
    };
    return fact;
}
uint64_t pages(const model::HiCacheState & state, model::HiCacheEffectType type, bool completed = false) {
    uint64_t total = 0;
    for (const auto & decision : state.effect_decision_ledger().decisions)
        if (decision.effect_type == type) total += completed ? decision.completed_page_count : decision.effective_page_count;
    return total;
}
void wait_for_all_victims(bool parent_child) {
    auto config = timing_config();
    config.write_policy = "write_back";
    config.l1_capacity_pages = parent_child ? 3 : 4;
    model::HiCacheState state(config);
    if (parent_child) {
        seed(state, 1, 0, 32);
        seed(state, 2, 0, 16); // Split the first path into a parent and child.
        seed(state, 3, 100, 16);
    }
    else {
        for (uint32_t i = 0; i < 4; ++i) seed(state, i + 1, i * 16, 16);
    }
    state.begin_formal_window(true);
    auto fact = extend();
    state.apply_fact(fact, HiCacheFactRole::CacheExtendInput);
    const auto writes = state.pending_device_writes(fact);
    require(state.allocation_pending(fact) && writes.size() == 2, "select and submit multiple victims before waiting");
    const auto * eviction = state.eviction_work(fact);
    require(eviction && eviction->victims.size() == 2
                && std::ranges::all_of(eviction->victims,
                                       [](const auto & victim) { return victim.release_after_write && victim.backed_up && victim.page_count == 1; }),
            "target-selected dirty victims explicitly require post-write release");
    require(writes[0].node != writes[1].node, "equal-sized writes preserve distinct target victim identities");
    for (const auto & victim : eviction->victims) {
        const auto count = std::ranges::count_if(writes, [&](const auto & write) {
            return write.node == victim.node && write.header.pages.size() == victim.page_count;
        });
        require(count == 1, "each selected victim has its own target write, independent of payload-size ties");
    }
    require(state.prefill_work_items().empty() && state.allocator_work_items().empty(), "waiting does not publish allocation or compute work");
    if (parent_child) {
        runtime::HiCacheTargetPager pager(config);
        const auto original = path("cache_lifecycle_commit", "first", 1, 0, 32);
        const auto expected = pager.project(original, original.full_path_tokens).page_ids();
        std::set<std::string> actual;
        for (const auto & write : writes) actual.insert(write.header.pages.begin(), write.header.pages.end());
        require(actual == std::set<std::string>(expected.begin(), expected.end()), "selected child makes its parent eligible without early GPU release");
    }
    fact.ts = 1'000;
    require(state.write_completion_count(fact) == 0, "elapsed estimated service alone is not device completion");
    hicache_timing_fixture::require_throws<std::logic_error>([&] { state.resume_allocation(fact); }, "resume requires actual device completion callbacks");

    fact.ts = writes.front().schedule.ready_ts;
    state.complete_device_write(fact, writes.front().header.operation_id);
    require(state.write_completion_count(fact) == 1, "first device completion makes only one queued write ready");
    require(state.unacknowledged_device_writes(fact).size() == 2 && state.pending_device_writes(fact).size() == 1,
            "ACK planning includes completed-but-unconfirmed writes as well as DMA still in flight");
    hicache_timing_fixture::require_throws<std::logic_error>([&] { state.resume_allocation(fact); },
                                                             "one completed victim cannot resume a multi-victim eviction");
    require(state.prefill_work_items().empty(), "failed resume cannot publish prefill work");

    fact.ts = writes.back().schedule.ready_ts;
    state.complete_device_write(fact, writes.back().header.operation_id);
    require(pages(state, model::HiCacheEffectType::CommitDeviceToHost, true) == 0 && pages(state, model::HiCacheEffectType::CommitHostToStorage) == 0,
            "DMA completion alone neither publishes host pages nor starts storage backup");
    state.confirm_allocation_writes(fact);
    require(state.allocation_pending(fact) && state.prefill_work_items().empty() && state.allocator_work_items().empty(),
            "CPU write confirmation does not skip the following device-page releases");
    require(state.write_completion_count(fact) == 0 && pages(state, model::HiCacheEffectType::CommitDeviceToHost, true) == 2,
            "confirmed writes leave the acknowledgement queue while allocation still waits");
    require(state.unacknowledged_device_writes(fact).empty(), "CPU confirmation removes both writes from the confirmation plan");
    fact.ts += 100;
    state.resume_allocation(fact);
    require(state.eviction_work(fact)->victims.size() == 2, "resuming must not append duplicate release work");
    require(!state.allocation_pending(fact) && state.prefill_work_items().size() == 1 && state.allocator_work_items().size() == 1,
            "all writes confirmed: release GPU pages and continue the original extend once");
    require(pages(state, model::HiCacheEffectType::CommitDeviceToHost, true) == 2 && pages(state, model::HiCacheEffectType::CommitHostToStorage) == 2,
            "confirmation publishes host and submits the independent storage backup");
}
void rejected_host_reservation_keeps_searching() {
    auto config = timing_config();
    config.write_policy = "write_back";
    config.l1_capacity_pages = 3;
    config.l2_capacity_pages = 1;
    model::HiCacheState state(config);
    seed(state, 1, 0, 32); // Oldest victim is too large for the host pool.
    seed(state, 2, 100, 16);
    state.begin_formal_window(true);
    auto fact = extend();
    state.apply_fact(fact, HiCacheFactRole::CacheExtendInput);
    const auto writes = state.pending_device_writes(fact);
    require(writes.size() == 1 && writes.front().header.pages.size() == 1, "try the next victim after a host reservation fails");
    require(state.eviction_work(fact)->victims.size() == 1 && state.eviction_work(fact)->victims.front().page_count == 1,
            "failed host reservation does not invent successful release work");
    const auto & rejected = state.eviction_work(fact)->rejected_candidates;
    require(rejected.size() == 1 && rejected.front().reason == "host_backup_rejected"
                && rejected.front().before_victim == 0
                && rejected.front().node != state.eviction_work(fact)->victims.front().node,
            "failed reservation retains its identity and position before the successful victim");
    require(rejected.front().heap_size == 2, "rejection records the candidate set before popping, not remaining victims");
    require(writes.front().node == state.eviction_work(fact)->victims.front().node,
            "a rejected reservation cannot displace the successful victim's write identity");
    fact.ts = writes.front().schedule.ready_ts;
    state.complete_device_write(fact, writes.front().header.operation_id);
    state.resume_allocation(fact);
    require(state.allocator_work_items().back().allocated_pages == 1, "actual extend may fit even if conservative eviction budget was not met");
}
void eviction_without_write_has_work() {
    for (const auto [capacity, backed_up] : std::vector<std::pair<uint64_t, bool>>{
             { 4,  true },
             { 8,  true },
             { 4, false }
    }) {
        auto config = timing_config();
        config.l1_capacity_pages = capacity;
        config.write_through_threshold = backed_up ? 1 : 1'000;
        model::HiCacheState state(config);
        for (uint32_t i = 0; i < 4; ++i) seed(state, i + 1, i * 16, 16);
        state.begin_formal_window(true);
        const auto fact = extend();
        state.apply_fact(fact, HiCacheFactRole::CacheExtendInput);
        require(!state.allocation_pending(fact) && state.pending_device_writes(fact).empty(), "write-through eviction does not create a DMA wait");
        const auto * work = state.eviction_work(fact);
        if (capacity == 4) {
            require(work && work->requested_pages == 2 && work->victims.size() == 2, "no pending DMA is not the same as no eviction control work");
            require(std::ranges::all_of(
                        work->victims,
                        [&](const auto & victim) { return !victim.release_after_write && victim.backed_up == backed_up && victim.page_count == 1; }),
                    "selection-phase release keeps the actual backed-up or regular path");
        }
        else require(!work, "the same inputs with enough target capacity must not inherit another configuration's eviction count");
        auto other = fact;
        other.cache_scope = "another-cache";
        require(!state.eviction_work(other), "eviction work cannot cross cache scopes");
        state.begin_formal_window(true);
        require(!state.eviction_work(fact), "window boundary discards earlier execution work");
    }
}
void lookup_keeps_host_input_until_capacity_returns() {
    auto config = timing_config();
    config.write_policy = "write_back";
    config.l1_capacity_pages = 2;
    model::HiCacheState state(config);
    seed(state, 1, 0, 32);
    seed(state, 50, 100, 32); // First path is now backed on host, second occupies GPU.
    state.begin_formal_window(true);
    auto candidate = path("prefetch_candidate_anchor", "reload", 100, 0, 32);
    state.apply_fact(candidate, HiCacheFactRole::PrefetchCandidateAnchor);
    auto lookup = path("cache_lookup_input", "reload", 110, 0, 32);
    state.apply_fact(lookup, HiCacheFactRole::CacheLookupInput);
    require(state.allocation_pending(lookup), "loadback must wait when GPU capacity requires a dirty eviction");
    require(state.load_allocation_will_succeed(lookup), "selected target releases make the retry fit without publishing it");
    require(state.allocation_pending(lookup) && !state.load_admission_work(lookup)->allocated,
            "projecting the retry cannot release pages or consume the pending allocation");
    require(!state.submit_loadbacks(lookup), "waiting lookup cannot submit H2D early");
    const auto writes = state.pending_device_writes(lookup);
    require(writes.size() == 1, "lookup submits the required writeback once");
    lookup.ts = writes.front().schedule.ready_ts;
    state.complete_device_write(lookup, writes.front().header.operation_id);
    require(!state.submit_loadbacks(lookup), "device completion must still pass the blocking confirmation");
    state.resume_allocation(lookup);
    require(state.load_admission_work(lookup)->allocated && state.load_allocation_will_succeed(lookup),
            "actual allocation return agrees with the read-only retry projection");
    const auto load = state.submit_loadbacks(lookup);
    require(load && load->operations.size() == 1 && load->io_schedule.effective_byte_count == 32,
            "resumed lookup allocates and submits the protected host prefix");
}
void target_eviction_order() {
    using Kind = runtime::HiCacheEvictionRegion::Kind;
    using Phase = runtime::HiCacheEvictionRegion::ControlPhase;
    model::HiCacheEvictionWork work;
    work.victims = { { 10, 1, false, true }, { 20, 1, true, true }, { 30, 1, true, true } };
    const auto sequence = runtime::eviction_sequence(work, true);
    require(sequence[2].preceding_kind == Kind::ReleaseBackup && sequence[4].preceding_kind == Kind::WriteBackup
                && sequence[6].control_phase == Phase::SelectionEnd && sequence[6].preceding_kind == Kind::WriteBackup,
            "equal-sized victims with different operations require different following control templates");
    std::vector<Kind> calls;
    std::vector<std::optional<size_t>> objects;
    std::vector<Phase> controls;
    for (const auto & step : sequence) {
        if (step.kind == Kind::Control) {
            controls.push_back(step.control_phase);
            require(!step.victim, "whole-loop control must not acquire a donor or target object identity");
        }
        else {
            calls.push_back(step.kind);
            objects.push_back(step.victim);
        }
    }
    require(calls == std::vector<Kind>{ Kind::ReleaseBackup, Kind::WriteBackup, Kind::WriteBackup, Kind::BlockingCheck,
                                       Kind::ReleaseBackup, Kind::ReleaseBackup }
                && objects == std::vector<std::optional<size_t>>{ 0, 1, 2, std::nullopt, 1, 2 },
            "target selection order determines writes and releases; equal page counts cannot swap object identity");
    require(controls == std::vector<Phase>{ Phase::Setup, Phase::Selection, Phase::Selection, Phase::SelectionEnd,
                                           Phase::ReleaseStart, Phase::ReleaseNext, Phase::Finish },
            "setup and finish run once while each loop has its own target-dependent repetition count");
    work.victims = { { 40, 1, false, false }, { 50, 1, false, true } };
    const auto write_through = runtime::eviction_sequence(work, false);
    require(write_through.size() == 5 && write_through[1].kind == Kind::ReleaseRegular && write_through[3].kind == Kind::ReleaseBackup
                && std::ranges::none_of(write_through, [](const auto & step) { return step.kind == Kind::BlockingCheck; }),
            "write-through eviction keeps regular/backup release paths without a write-back wait");
    work.victims.front().backed_up = true;
    const auto clean_write_back = runtime::eviction_sequence(work, true);
    require(clean_write_back[4].control_phase == Phase::SelectionEnd && clean_write_back[4].preceding_kind == Kind::ReleaseBackup,
            "selection ending with a release must not borrow a write-return tail");
    require(std::ranges::count(clean_write_back, Kind::BlockingCheck, &runtime::HiCacheEvictionStep::kind) == 1
                && std::ranges::none_of(clean_write_back, [](const auto & step) { return step.control_phase == Phase::ReleaseStart; }),
            "a write-back eviction still checks existing writes even when its selected victims need no new writes");
    work.victims.clear();
    require(runtime::eviction_sequence(work, false).size() == 1 && runtime::eviction_sequence(work, true).size() == 3,
            "empty selection preserves policy-specific control instead of inventing a victim");
    work.victims = {{40, 1, false, false}, {50, 1, false, true}};
    work.rejected_candidates = {{60, 0, "locked_node"}, {61, 1, "locked_node"}, {62, 1, "locked_node"}, {63, 2, "locked_node"}};
    const auto skipped = runtime::eviction_sequence(work, false);
    std::vector<Kind> ordered;
    for (const auto & step : skipped) ordered.push_back(step.kind);
    require(ordered == std::vector<Kind>{Kind::Control, Kind::SkipLocked, Kind::ReleaseRegular, Kind::Control,
                Kind::SkipLocked, Kind::SkipLocked, Kind::ReleaseBackup, Kind::SkipLocked, Kind::Control},
            "locked candidates retain their before/between/after-victim order without extra successful control templates");
    const auto skipped_write_back = runtime::eviction_sequence(work, true);
    for (size_t i = 0; i < work.rejected_candidates.size(); ++i)
        work.rejected_candidates[i].heap_size = 8 - i;
    size_t skip_index = 0;
    for (const auto & step : runtime::eviction_sequence(work, false)) {
        if (step.kind != Kind::SkipLocked) continue;
        require(step.heap_size == 8 - skip_index++, "skip retains its actual pre-pop candidate count");
    }
    require(skip_index == work.rejected_candidates.size(), "all skip sizes survive sequencing");
    require(std::abs(runtime::locked_candidate_cost(1, 0.2, 0.05) - 0.2) < 1e-12,
            "single candidate pays fixed work only");
    require(std::abs(runtime::locked_candidate_cost(16, 0.2, 0.05) - 0.4) < 1e-12,
            "heap work scales by height, not page size or victim count");
    require(runtime::locked_candidate_cost(0, 0.2, 0) == 0.2, "explicit constant costs need no heap estimate");
    hicache_timing_fixture::require_throws<std::invalid_argument>([&] { (void)runtime::locked_candidate_cost(0, 0.2, 0.05); },
                                                                  "nonconstant skip cost cannot guess missing heap size");
    const auto selection_end = std::ranges::find(skipped_write_back, Phase::SelectionEnd, &runtime::HiCacheEvictionStep::control_phase);
    require(selection_end != skipped_write_back.end() && selection_end->preceding_kind == Kind::ReleaseBackup,
            "trailing locked candidates do not replace the last actual operation in template matching");
    work.victims.clear();
    work.rejected_candidates = {{60, 0, "locked_node"}};
    require(runtime::eviction_sequence(work, false).size() == 2,
            "an all-skipped heap must retain both empty-branch control and skip work");
    for (int invalid = 0; invalid < 3; ++invalid) {
        work.victims = {{40, 1, false, false}};
        work.rejected_candidates = {{60, 0, "locked_node"}};
        if (invalid == 0) work.rejected_candidates.front().reason = "host_backup_rejected";
        if (invalid == 1) work.rejected_candidates.front().before_victim = 2;
        if (invalid == 2) work.rejected_candidates = {{60, 1, "locked_node"}, {61, 0, "locked_node"}};
        hicache_timing_fixture::require_throws<std::invalid_argument>([&] { (void)runtime::eviction_sequence(work, false); },
                                                                      "unmodeled or out-of-order rejection cannot become a locked skip");
    }
}

void device_dependencies_move_the_continuation() {
    for (const auto [device_busy, release_us] : std::vector<std::pair<uint64_t, uint64_t>>{ { 0, 0 }, { 7, 0 }, { 0, 11 }, { 7, 11 } }) {
        auto config = timing_config();
        config.write_policy = "write_back";
        model::HiCacheState state(config);
        for (uint32_t i = 0; i < 4; ++i) seed(state, i + 1, i * 16, 16);
        state.begin_formal_window(true);
        core::DagGraph graph;
        const auto prior = graph.add_synthetic_node({ .name = "prior device work", .is_cpu = false, .duration = device_busy });
        const auto entry = graph.add_synthetic_node({ .name = "allocation entry" });
        const auto next = graph.add_synthetic_node({ .name = "CPU after allocation", .duration = 5, .counts_toward_e2e = true });
        graph.add_edge(entry, next, core::DagEdgeKind::Mutation);
        auto fact = extend();
        std::map<size_t, std::string> completions;
        std::optional<size_t> confirm_at, release_at, resume_at;
        uint64_t resumed = 0;
        (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t at, simulation::FutureDag & future) {
            fact.ts = 100 + at;
            fact.execution_anchor_node_id = node;
            if (node == entry) {
                state.apply_fact(fact, HiCacheFactRole::CacheExtendInput);
                require(state.allocation_pending(fact), "synthetic allocation must really wait for two writes");
                auto previous = prior;
                for (const auto & write : state.pending_device_writes(fact)) {
                    const auto dma = future.append({ .name = "D2H", .is_cpu = false, .duration = write.schedule.duration_us });
                    const auto done = future.append({ .name = "device completion", .is_cpu = false });
                    future.depend(previous, dma);
                    future.depend(dma, done);
                    completions.emplace(done, write.header.operation_id);
                    previous = done;
                }
                const auto confirmation = future.append({ .name = "CPU write confirmation", .duration = 3 });
                confirm_at = future.append({ .name = "write check return" });
                release_at = future.append({ .name = "release victim pages", .duration = release_us });
                resume_at = future.append({ .name = "capacity guard return" });
                future.depend(previous, confirmation);
                future.depend(confirmation, *confirm_at);
                future.depend(*confirm_at, *release_at);
                future.depend(*release_at, *resume_at);
                future.depend(*resume_at, next);
            }
            if (const auto found = completions.find(node); found != completions.end()) state.complete_device_write(fact, found->second);
            if (confirm_at && node == *confirm_at) {
                state.confirm_allocation_writes(fact);
                require(state.allocation_pending(fact) && state.prefill_work_items().empty(), "acknowledgement alone must not continue allocation");
                require(pages(state, model::HiCacheEffectType::CommitDeviceToHost, true) == 2
                            && pages(state, model::HiCacheEffectType::CommitHostToStorage) == 2,
                        "host publication and independent storage submission stay at ACK, before device-page release returns");
            }
            if (release_at && node == *release_at)
                require(state.allocation_pending(fact) && state.write_completion_count(fact) == 0,
                        "release runs after acknowledged writes but before capacity continuation");
            if (resume_at && node == *resume_at) {
                state.resume_allocation(fact);
                resumed = at;
            }
            if (node == next)
                require(!state.allocation_pending(fact) && state.prefill_work_items().size() == 1,
                        "CPU successor only starts after write confirmation and allocation continuation");
        });
        require(resumed == device_busy + 23 + release_us && graph.node(next).completion_time == device_busy + 28 + release_us,
                "device queue, CPU confirmation and release work all move allocation and the E2E successor");
        for (const auto & decision : state.effect_decision_ledger().decisions)
            if (decision.effect_type == model::HiCacheEffectType::CommitCapacityGate)
                require(decision.effective_page_count == 2 && decision.consumer_boundary.timestamp_us == 100 + resumed,
                        "capacity dependency belongs to the actual continuation, not the submission time");
        hicache_timing_fixture::require_static_replay(graph, "expanded writeback graph replays exactly without model callbacks");
    }
}
} // namespace

void check_writeback_execution() {
    {
        using Phase = runtime::HiCacheEvictionRegion::ControlPhase;
        const std::vector<runtime::EvictionControlCostSample> samples{
            {Phase::Selection, 2, 4}, {Phase::Selection, 4, 2}, {Phase::Finish, 9, 0}};
        const auto selection = runtime::estimate_eviction_control_cost(samples, Phase::Selection);
        require(selection && selection->samples == 2 && selection->cpu_us == 3 && selection->residual_us == 3
                    && !selection->phase_extrapolated, "control cost uses phase evidence without requiring a source predecessor or graph template");
        const auto unseen = runtime::estimate_eviction_control_cost(samples, Phase::SelectionEnd);
        require(unseen && unseen->samples == 3 && unseen->cpu_us == 5 && unseen->residual_us == 2
                    && unseen->phase_extrapolated, "unseen phase uses an explicit pooled base estimate, retaining residual cost separately");
        require(!runtime::estimate_eviction_control_cost({}, Phase::Selection), "missing cost evidence is not a zero-cost operation");
        auto mixed = samples;
        mixed.push_back({Phase::Selection, 100, 80, true});
        mixed.push_back({Phase::SelectionEnd, 200, 90, true});
        const auto preferred = runtime::estimate_eviction_control_cost(mixed, Phase::Selection);
        require(preferred->cpu_us == 3 && preferred->samples == 2 && !preferred->independent,
                "independent sample count must not replace compatible base costs");
        const auto extrapolated = runtime::estimate_eviction_control_cost(mixed, Phase::SelectionEnd);
        require(extrapolated->cpu_us == 5 && extrapolated->phase_extrapolated && !extrapolated->independent,
                "supported base phase extrapolation precedes independent phase costs");
        const std::vector<runtime::EvictionControlCostSample> shared{{Phase::Selection, 7, 2, true}};
        const auto fallback = runtime::estimate_eviction_control_cost(shared, Phase::Selection);
        require(fallback->cpu_us == 7 && fallback->independent,
                "independent costs remain usable when no eligible base sample exists");
        const std::vector<runtime::EvictionControlCostSample> invalid{{Phase::Selection, -1, 0}};
        hicache_timing_fixture::require_throws<std::invalid_argument>([&] { (void)runtime::estimate_eviction_control_cost(invalid, Phase::Selection); },
                                                                      "negative control costs must not enter target work");
    }
    lifecycle_cursor_waits_for_whole_target_nodes();
    lifecycle_return_is_separate_from_write_submission();
    chunked_insert_does_not_count_as_a_write_hit();
    target_eviction_order();
    eviction_without_write_has_work();
    wait_for_all_victims(false);
    wait_for_all_victims(true);
    rejected_host_reservation_keeps_searching();
    lookup_keeps_host_input_until_capacity_returns();
    device_dependencies_move_the_continuation();
}
