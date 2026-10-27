#include "markov/trace_graph/modules/hicache/runtime/eviction_sequence.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

std::string eviction_control_name(HiCacheEvictionRegion::ControlPhase phase) {
    using Phase = HiCacheEvictionRegion::ControlPhase;
    switch (phase) {
    case Phase::Setup:
        return "setup";
    case Phase::Selection:
        return "selection";
    case Phase::SelectionEnd:
        return "selection_end";
    case Phase::ReleaseStart:
        return "release_start";
    case Phase::ReleaseNext:
        return "release_next";
    case Phase::Finish:
        return "finish";
    case Phase::Empty:
        return "empty";
    case Phase::None:
        break;
    }
    throw std::invalid_argument("Eviction control requires a semantic phase");
}

std::vector<HiCacheEvictionStep> eviction_sequence(const model::HiCacheEvictionWork & work, bool write_back) {
    using Kind = HiCacheEvictionRegion::Kind;
    using Phase = HiCacheEvictionRegion::ControlPhase;
    std::vector<HiCacheEvictionStep> steps;
    const auto control = [&](Phase phase) {
        std::optional<Kind> preceding;
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            if (it->kind == Kind::Control || it->kind == Kind::SkipLocked) continue;
            preceding = it->kind;
            break;
        }
        steps.push_back({ Kind::Control, phase, std::nullopt, preceding });
    };
    size_t skipped = 0, previous = 0;
    for (const auto & rejected : work.rejected_candidates) {
        if ((rejected.reason != "locked_node" && rejected.reason != "host_backup_rejected") || rejected.before_victim < previous
            || rejected.before_victim > work.victims.size())
            throw std::invalid_argument("Unsupported or out-of-order rejected eviction candidate: " + rejected.reason);
        previous = rejected.before_victim;
    }
    bool first_call = true;
    const auto selection = [&] {
        if (!first_call) control(Phase::Selection);
        first_call = false;
    };
    const auto skips_before = [&](size_t victim) {
        while (skipped < work.rejected_candidates.size() && work.rejected_candidates[skipped].before_victim == victim) {
            const auto & rejected = work.rejected_candidates[skipped];
            if (rejected.reason == "host_backup_rejected") {
                if (!write_back) throw std::invalid_argument("Rejected write-back requires write-back policy");
                selection();
                steps.push_back({ Kind::RejectedBackup });
                steps.back().rejected_candidate = skipped;
            }
            else {
                steps.push_back({ Kind::SkipLocked });
                steps.back().heap_size = rejected.heap_size;
            }
            ++skipped;
        }
    };
    const bool has_write_attempt =
        std::ranges::any_of(work.rejected_candidates, [](const auto & rejected) { return rejected.reason == "host_backup_rejected"; });
    if (work.victims.empty() && !has_write_attempt) {
        control(Phase::Empty);
        skips_before(0);
        if (write_back) {
            steps.push_back({ Kind::BlockingCheck });
            control(Phase::Finish);
        }
        return steps;
    }
    control(Phase::Setup);
    for (size_t i = 0; i < work.victims.size(); ++i) {
        const auto & victim = work.victims[i];
        if (victim.release_after_write && !write_back) throw std::invalid_argument("Post-write release requires write-back policy");
        skips_before(i);
        selection();
        const auto kind = victim.release_after_write ? Kind::WriteBackup : victim.backed_up ? Kind::ReleaseBackup : Kind::ReleaseRegular;
        steps.push_back({ kind, Phase::None, i });
    }
    skips_before(work.victims.size());
    if (write_back) {
        control(Phase::SelectionEnd);
        steps.push_back({ Kind::BlockingCheck });
        bool first = true;
        for (size_t i = 0; i < work.victims.size(); ++i) {
            if (!work.victims[i].release_after_write) continue;
            control(first ? Phase::ReleaseStart : Phase::ReleaseNext);
            steps.push_back({ Kind::ReleaseBackup, Phase::None, i });
            first = false;
        }
    }
    control(Phase::Finish);
    return steps;
}

} // namespace markov::trace_graph::modules::hicache::runtime
