#include "markov/trace_graph/modules/hicache/runtime/eviction_sequence.hpp"
#include <stdexcept>
#include <cmath>
#include <algorithm>

namespace markov::trace_graph::modules::hicache::runtime {

std::optional<EvictionControlCost> estimate_eviction_control_cost(
    std::span<const EvictionControlCostSample> samples, HiCacheEvictionRegion::ControlPhase phase) {
    if (samples.empty()) return std::nullopt;
    const bool has_base = std::ranges::any_of(samples, [](const auto& sample) { return !sample.independent; });
    EvictionControlCost matched, pooled;
    for (const auto & sample : samples) {
        if (!std::isfinite(sample.cpu_us) || !std::isfinite(sample.residual_us) || sample.cpu_us < 0 || sample.residual_us < 0)
            throw std::invalid_argument("Invalid base eviction control cost sample");
        if (has_base && sample.independent) continue;
        pooled.cpu_us += sample.cpu_us; pooled.residual_us += sample.residual_us; ++pooled.samples;
        if (sample.phase == phase) {
            matched.cpu_us += sample.cpu_us; matched.residual_us += sample.residual_us; ++matched.samples;
        }
    }
    auto cost = matched.samples ? matched : pooled;
    cost.phase_extrapolated = !matched.samples;
    cost.independent = !has_base;
    cost.cpu_us /= cost.samples; cost.residual_us /= cost.samples;
    return cost;
}

double locked_candidate_cost(size_t heap_size, double fixed_us, double log2_heap_us) {
    if (!std::isfinite(fixed_us) || fixed_us <= 0 || !std::isfinite(log2_heap_us) || log2_heap_us < 0)
        throw std::invalid_argument("Invalid locked-candidate CPU coefficients");
    if (log2_heap_us == 0) return fixed_us;
    if (!heap_size) throw std::invalid_argument("Locked-candidate CPU cost requires the pre-pop heap size");
    return fixed_us + log2_heap_us * std::log2(static_cast<double>(heap_size));
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
        if (rejected.reason != "locked_node" || rejected.before_victim < previous || rejected.before_victim > work.victims.size())
            throw std::invalid_argument("Unsupported or out-of-order rejected eviction candidate: " + rejected.reason);
        previous = rejected.before_victim;
    }
    const auto skips_before = [&](size_t victim) {
        while (skipped < work.rejected_candidates.size() && work.rejected_candidates[skipped].before_victim == victim) {
            steps.push_back({Kind::SkipLocked});
            steps.back().heap_size = work.rejected_candidates[skipped].heap_size;
            ++skipped;
        }
    };
    if (work.victims.empty()) {
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
        if (i) control(Phase::Selection);
        skips_before(i);
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
