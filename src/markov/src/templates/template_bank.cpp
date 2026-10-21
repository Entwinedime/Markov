#include "markov/templates/templates.hpp"

#include "markov/templates/kv_workload.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace markov {
namespace {

using Json = nlohmann::json;

} // namespace

TemplateBank::TemplateBank(TemplateSource source) : source_(std::move(source)) {
    for (auto & [key, definitions] : source_.definitions)
        for (auto & definition : definitions) definition.source_manifest = source_.manifest_path;
    bind_sources({});
}

bool TemplateBank::shares_source_contract(const TemplateBank & bank) const {
    const auto & source = bank.source_;
    return source.model_configuration == source_.model_configuration && source.configuration == source_.configuration
           && source.rank_bindings == source_.rank_bindings && source.page_size_tokens == source_.page_size_tokens
           && source.sequence_capacity_tokens == source_.sequence_capacity_tokens && *source.admission_policy == *source_.admission_policy
           && source.resolution == source_.resolution && source.deployment.shares_implementation(source_.deployment)
           && source.deployment.measurement == source_.deployment.measurement;
}

void TemplateBank::bind_sources(std::span<const TemplateBank * const> banks, std::span<const TemplateDefinition * const> operation_sources,
                                std::optional<std::uint32_t> request_capacity) {
    operation_sources_.assign(operation_sources.begin(), operation_sources.end());
    donors_.clear();
    banks_ = { this };
    for (const auto & [key, definitions] : source_.definitions)
        for (const auto & definition : definitions) donors_[key].push_back(&definition);

    for (const auto * bank : banks) {
        if (bank == this) continue;
        // Local rewrites may share observations under the same native setup.
        // Configuration migration remains a separate, explicitly qualified rule.
        if (!shares_source_contract(*bank)) continue;

        banks_.push_back(bank);
        for (const auto & [key, definitions] : bank->source_.definitions)
            for (const auto & definition : definitions) donors_[key].push_back(&definition);
    }

    // Each role contributes its own post-warmup state. A root source cannot
    // initialize resources whose native worker role was never observed.
    workspace_pools_.clear();
    const auto capacity = request_capacity.value_or(source_.request_pool_domain.maximum);
    for (std::uint32_t rank = 0; rank < source_.rank_count(); ++rank) {
        const auto role = source_.rank_bindings[rank].source_rank;
        const auto donor = std::ranges::find_if(banks_, [&](const auto * bank) {
            return bank->source_.source_rank == role && bank->qualify_initialization(capacity).has_value();
        });
        if (donor == banks_.end()) continue;
        for (auto pool : (*donor)->source_.workspace_pools) {
            pool.resource.rank = rank;
            workspace_pools_.push_back(pool);
        }
    }
}

std::expected<std::vector<TemplateOperationRewrite>, std::string> TemplateBank::plan_batch_operations(const TemplateDefinition & source,
                                                                                                      const BatchInput & target) const {
    std::vector<TemplateOperationRewrite> rewrites;
    for (const auto & operation : source.operations) {
        const auto observed = source.batch_operations.find(operation.host.source_span);
        if (observed == source.batch_operations.end()) continue;
        if (observed->second.source_requests != source.source_requests.size()) return std::unexpected("batch_operation_request_relation_not_qualified");
        if (observed->second.accepts(target)) continue;
        if (target.requests.empty()) return std::unexpected("batch_operation_empty_target_not_supported");

        struct OperationSource {
            const TemplateDefinition * definition;
            bool rebindable_only;
        };
        std::vector<OperationSource> candidates;
        for (const auto & [key, definitions] : donors_) {
            const auto & [step, phase, rank] = key;
            if (rank != source.rank) continue;
            // Page-table division has the same local semantics in both model
            // phases. Other operations retain their original phase contract.
            if (observed->second.role == BatchOperationRole::PageTableDivide) {
                if (step != StepKind::Prefill && step != StepKind::Decode) continue;
            }
            else if (step != source.step || phase != target.phase) continue;
            for (const auto & definition : definitions) candidates.push_back({ definition, false });
        }

        // Fragment sources never enter the whole-template or state-donor banks.
        for (const auto * definition : operation_sources_) {
            if (definition->rank != source.rank || definition->step != source.step || definition->phase != target.phase) continue;
            if (std::ranges::find(candidates, definition, &OperationSource::definition) != candidates.end()) continue;
            candidates.push_back({ definition, true });
        }

        std::optional<TemplateOperationRewrite> selected;
        std::string gap = "batch_operation_variant_not_observed";
        for (const auto & candidate : candidates) {
            const auto & donor = *candidate.definition;
            for (const auto & variant : donor.operations) {
                const auto replacement = donor.batch_operations.find(variant.host.source_span);
                if (replacement == donor.batch_operations.end() || replacement->second.role != observed->second.role
                    || replacement->second.source_requests != donor.source_requests.size() || !replacement->second.accepts(target)
                    || (candidate.rebindable_only && !replacement->second.request_pool_rebindable))
                    continue;
                auto plan = source.plan_operation_replacement(operation, donor, variant);
                if (!plan) {
                    gap = plan.error();
                    continue;
                }
                selected = std::move(*plan);
                break;
            }
            if (selected) break;
        }
        if (!selected) return std::unexpected(gap);
        rewrites.push_back(std::move(*selected));
    }
    return rewrites;
}

std::expected<std::optional<TemplateOperationRewrite>, std::string> TemplateBank::plan_kv_region(const TemplateDefinition & source,
                                                                                                 const BatchInput & target) const {
    if (!source.kv_region) return std::nullopt;
    const auto & branch = *source.kv_region;
    bool changes_pages = false;
    std::optional<std::size_t> released_members;
    switch (branch.kind) {
    case KvRegionKind::Allocation:
        changes_pages = KvBatchWork(target, source_.page_size_tokens).new_pages() != 0;
        break;
    case KvRegionKind::ReleaseGroup:
        released_members = std::ranges::count_if(target.requests, [](const auto & request) { return request.finishes; });
        changes_pages = *released_members != 0;
        break;
    }

    // A nonempty release can still change its operation structure: one member
    // uses a copy, while multiple members use a qualified tensor-list Cat.
    const bool same_members = !released_members || branch.accepts_members(*released_members);
    if (branch.changes_pages == changes_pages && same_members) return std::nullopt;

    std::string gap = "kv_region_variant_not_observed";
    for (const auto * candidate : donors_.at({ source.step, source.phase, source.rank })) {
        const auto & donor = *candidate;
        if (!donor.kv_region || donor.kv_region->kind != branch.kind || donor.kv_region->changes_pages != changes_pages) continue;
        if (released_members && !donor.kv_region->accepts_members(*released_members)) continue;

        auto replacement = source.plan_region_replacement(source.regions[source.kv_region->region], donor, donor.regions[donor.kv_region->region]);
        if (replacement) return std::move(*replacement);
        gap = replacement.error();
    }
    return std::unexpected(gap);
}

std::expected<std::optional<TemplateOperationRewrite>, std::string> TemplateBank::plan_transition(const TemplateDefinition & source,
                                                                                                  const BatchInput & target) const {
    if (source.step != StepKind::Schedule || source.matches_transition(target)) return std::nullopt;
    if (!source.transition_region) return std::unexpected("batch_transition_region_not_qualified");

    std::string gap = "batch_transition_variant_not_observed";
    // The qualified non-merge shell only updates scalar membership/latch state.
    // Replace its inner filtering call, retaining the surrounding adoption path.
    if (source.transition_cpu_parameters && source.transition->pending_prefill_requests && target.transition.pending_prefill_requests && !target.merge) {
        const auto & filter = target.filters.front();
        if (!filter.retained_indices.empty() && filter.retained_indices.size() < filter.input_count) {
            auto plan = plan_filter(source, BatchFilterBinding::Transition, target);
            if (plan) return std::move(*plan);
            gap = plan.error();
        }
    }

    for (const auto & [key, candidates] : donors_) {
        if (std::get<0>(key) != source.step || std::get<2>(key) != source.rank) continue;
        for (const auto * candidate : candidates) {
            const auto & donor = *candidate;
            if (!donor.transition_region || !donor.matches_transition(target)) continue;
            auto plan = source.plan_region_replacement(source.regions[*source.transition_region], donor, donor.regions[*donor.transition_region]);
            if (plan) return std::move(*plan);
            gap = plan.error();
        }
    }
    return std::unexpected(gap);
}

std::expected<TemplateOperationRewrite, std::string> TemplateBank::plan_filter(const TemplateDefinition & source, BatchFilterBinding role,
                                                                               const BatchInput & target) const {
    const auto recipient = source.filter_regions.find(role);
    if (recipient == source.filter_regions.end()) return std::unexpected("batch_filter_region_not_qualified");

    std::string gap = "batch_filter_variant_not_qualified";
    for (const auto * donor : donors_.at({ source.step, source.phase, source.rank })) {
        for (const auto & [donor_role, region] : donor->filter_regions) {
            if (!donor->matches_partial_filter(donor_role, role, target)) continue;
            auto plan = source.plan_region_replacement(source.regions[recipient->second], *donor, donor->regions[region]);
            if (!plan) {
                gap = plan.error();
                continue;
            }

            for (auto & node : plan->inserted_nodes) node.filter_binding = role;
            return std::move(*plan);
        }
    }
    return std::unexpected(gap);
}

std::expected<std::optional<TemplateOperationRewrite>, std::string> TemplateBank::plan_running_update(const TemplateDefinition & source,
                                                                                                      const BatchInput & target) const {
    if (source.step != StepKind::Schedule || source.phase == BatchPhase::Prefill) return std::nullopt;
    if (source.phase == BatchPhase::Idle) {
        const auto prefix_count = target.transition.pending_prefill_requests ? 1U : 0U;
        const bool has_update = target.filters.size() == prefix_count + 1;
        if (!source.running_update_region && !has_update) return std::nullopt;
        if (source.running_update_region && !has_update) {
            if (!target.requests.empty() || target.admission.running_requests != 0 || target.filters.size() != prefix_count)
                return std::unexpected("empty_running_update_not_qualified");

            // An already-empty running batch skips this call entirely. The
            // producer qualified a CPU-only empty return; retain its external
            // ordering points, but remove the observed work and internal edges.
            const auto & cut = source.regions[*source.running_update_region];
            const auto ports = source.region_ports(cut);
            if (!ports) return std::unexpected(ports.error());
            if (ports->size() != 1 || (ports->front().resource).domain != SourceDomain::CpuThread)
                return std::unexpected("empty_running_update_interface_not_qualified");
            TemplateOperationRewrite removal;
            removal.nodes = cut.nodes;
            removal.dependencies = cut.internal_dependencies;
            removal.added_dependencies.push_back({ SourceRelation::ThreadOrder, ports->front().entry, ports->front().exit });
            return removal;
        }
        if (!source.running_update_region || !has_update || !target.requests.empty() || target.filters.back().input_count == 0
            || !target.filters.back().retained_indices.empty())
            return std::unexpected("empty_running_update_not_qualified");

        // The producer qualified the empty CPU early return. Input membership
        // changes its work parameters, never adds a decode or a ratio decay.
        return std::nullopt;
    }
    if (!source.running_update_region) return std::unexpected("running_update_region_not_qualified");

    // The controller rejects retraction before reserving pages. Keep the same
    // domain when planning a template independently of controller execution.
    const KvBatchWork work(target, source_.page_size_tokens);
    if (!work.single_token_requests() || work.new_pages() > static_cast<std::int64_t>(target.free_kv_pages))
        return std::unexpected("decode_update_memory_path_not_supported");
    if (target.filters.empty() || target.filters.back().retained_indices.size() != target.requests.size())
        return std::unexpected("decode_update_membership_not_qualified");
    if (source.matches_running_filter(target)) return std::nullopt;

    std::string gap = "decode_update_variant_not_observed";
    for (const auto * candidate : donors_.at({ source.step, source.phase, source.rank })) {
        const auto & donor = *candidate;
        if (!donor.matches_running_filter(target)) continue;
        const auto & region = donor.regions[*donor.running_update_region];
        auto plan = source.plan_region_replacement(source.regions[*source.running_update_region], donor, region);
        if (plan) return std::move(*plan);
        gap = plan.error();
    }
    return std::unexpected(gap);
}

std::expected<std::vector<TemplateOperationRewrite>, std::string> TemplateBank::plan_publication_members(const TemplateDefinition & source,
                                                                                                         const BatchInput & target) const {
    std::vector<TemplateOperationRewrite> rewrites;
    if (!source.publication_sequence) return rewrites;
    const auto & source_members = source.sequences[*source.publication_sequence].source_members;

    // Reuse qualified member cuts across calls of the same phase/rank and
    // policy. Shared output and aggregate release remain with the recipient.
    std::vector<const TemplateDefinition *> donors{ &source };
    for (const auto * candidate : donors_.at({ source.step, source.phase, source.rank })) {
        if (candidate != &source && candidate->publication_sequence && source.publication_layout->can_reuse_members_from(*candidate->publication_layout))
            donors.push_back(candidate);
    }

    bool changed = source_members.size() != target.requests.size();
    std::vector<TemplateRegionReference> members;
    for (std::size_t slot = 0; slot < target.requests.size(); ++slot) {
        const auto finishes = target.requests[slot].finishes;
        if (slot < source_members.size() && source.publication_finishes[slot] == finishes) {
            members.push_back({ &source, *source.publication_sequence, slot });
            continue;
        }

        bool selected = false;
        std::string gap = "publication_member_branch_not_qualified";
        for (const auto * donor : donors) {
            const auto & donor_members = donor->sequences[*donor->publication_sequence].source_members;
            for (std::size_t donor_slot = 0; donor_slot < donor_members.size(); ++donor_slot) {
                if (donor->publication_finishes[donor_slot] != std::optional(finishes)) continue;
                // A member is selected by its own interface. Its predecessor
                // belongs to the assembled sequence, not the old recipient slot.
                const auto ports = donor->region_ports(donor->regions[donor_members[donor_slot]]);
                if (!ports) {
                    gap = ports.error();
                    continue;
                }

                members.push_back({ donor, *donor->publication_sequence, donor_slot });
                selected = true;
                break;
            }
            if (selected) break;
        }
        if (!selected) return std::unexpected(gap);

        changed = true;
    }
    if (changed) {
        auto sequence = source.plan_region_sequence(*source.publication_sequence, members);
        if (!sequence) return std::unexpected(sequence.error());
        rewrites.push_back(std::move(*sequence));
    }
    return rewrites;
}

std::expected<std::optional<TemplateOperationRewrite>, std::string> TemplateBank::plan_incoming_requests(const TemplateDefinition & source,
                                                                                                         const BatchInput & target) {
    if (source.step != StepKind::Schedule || source.source_incoming_requests.size() == target.incoming_requests.size()) return std::nullopt;
    if (!source.incoming_request_sequence) return std::unexpected("input_request_sequence_not_observed");

    const auto index = *source.incoming_request_sequence;
    const auto & sequence = source.sequences[index];
    std::vector<TemplateRegionReference> members;
    for (std::size_t slot = 0; slot < target.incoming_requests.size(); ++slot)
        members.push_back({ &source, index, std::min(slot, sequence.source_members.size() - 1) });
    return source.plan_region_sequence(index, members);
}

std::expected<void, std::string> TemplateBank::bind_input_reception(TemplateTopology & topology, const TemplateDefinition & source,
                                                                    const BatchInput & target) const {
    if (source.step != StepKind::Schedule || source.source_incoming_requests.empty() == target.incoming_requests.empty()) return {};
    if (!source.input_reception_sequence) return std::unexpected("input_reception_prefix_not_observed");

    std::string gap = "input_reception_variant_not_qualified";
    for (const auto & [key, donors] : donors_) {
        if (std::get<0>(key) != source.step || std::get<2>(key) != source.rank) continue;
        for (const auto * donor : donors) {
            if (!donor->input_reception || !*donor->input_reception || !donor->input_reception_sequence
                || donor->source_incoming_requests.empty() != target.incoming_requests.empty())
                continue;
            auto members = plan_incoming_requests(*donor, target);
            if (!members) {
                gap = members.error();
                continue;
            }
            TemplateTopology prepared(*donor);
            if (*members) {
                const auto expanded = prepared.apply_operations(*donor, std::span(&**members, 1));
                if (!expanded) {
                    gap = expanded.error();
                    continue;
                }
            }

            const auto index = *donor->input_reception_sequence;
            std::vector<TemplateRegionReference> prefix;
            for (std::size_t slot = 0; slot < donor->sequences[index].source_members.size(); ++slot) prefix.push_back({ donor, index, slot });
            const auto plan = source.plan_region_sequence(*source.input_reception_sequence, prefix);
            if (!plan) {
                gap = plan.error();
                continue;
            }
            const auto bound = topology.replace_sequence(source, *plan->sequence, *donor, prepared);
            if (bound) return {};
            gap = bound.error();
        }
    }
    return std::unexpected(gap);
}

std::expected<std::vector<TemplateOperationRewrite>, std::string> TemplateBank::plan_materializations(const TemplateDefinition & source,
                                                                                                      const BatchInput & target) const {
    auto kv_region = plan_kv_region(source, target);
    if (!kv_region) return std::unexpected(kv_region.error());

    auto operations = plan_batch_operations(source, target);
    if (!operations) return std::unexpected(operations.error());
    auto rewrites = std::move(*operations);

    if (source.source_incoming_requests.empty() == target.incoming_requests.empty()) {
        auto incoming = plan_incoming_requests(source, target);
        if (!incoming) return std::unexpected(incoming.error());
        if (*incoming) rewrites.push_back(std::move(**incoming));
    }

    // Production declares which copies become views for one request. The core
    // qualifies their cuts and preserves the declared producer's readiness.
    if (target.requests.size() == 1) {
        for (const auto & [operation, producer] : source.single_request_views) {
            auto removal = source.plan_view_materialization_removal(source.operations.at(operation), producer);
            if (!removal) return std::unexpected(removal.error());
            rewrites.push_back(std::move(*removal));
        }
    }

    if (source.source_requests.size() != target.requests.size()) {
        const bool admission = source.step == StepKind::Schedule && source.phase == BatchPhase::Prefill;
        const auto sequence_id = admission ? source.admission_sequence : source.request_initialization_sequence;
        if (admission) {
            if (!sequence_id || !target.admission_work.parameterized_cpu_path(target.admission, target.requests.size()))
                return std::unexpected("admission_member_sequence_not_qualified");
            // The first admission member initializes native state; only an
            // observed later member supplies the insertion branch for extra slots.
            if (target.requests.size() > 1 && source.sequences[*sequence_id].source_members.size() < 2)
                return std::unexpected("admission_insertion_member_not_observed");
        }
        if (sequence_id) {
            const auto & regions = source.sequences[*sequence_id].source_members;
            std::vector<TemplateRegionReference> members;
            for (std::size_t slot = 0; slot < target.requests.size(); ++slot) {
                members.push_back({ &source, *sequence_id, std::min(slot, regions.size() - 1) });
            }
            auto sequence = source.plan_region_sequence(*sequence_id, members);
            if (!sequence) return std::unexpected(sequence.error());
            rewrites.push_back(std::move(*sequence));
        }
    }

    auto publication = plan_publication_members(source, target);
    if (!publication) return std::unexpected(publication.error());
    for (auto & rewrite : *publication) rewrites.push_back(std::move(rewrite));

    auto update = plan_running_update(source, target);
    if (!update) return std::unexpected(update.error());
    if (*update) rewrites.push_back(std::move(**update));

    auto transition = plan_transition(source, target);
    if (!transition) return std::unexpected(transition.error());
    if (*transition) rewrites.push_back(std::move(**transition));

    if (*kv_region) rewrites.push_back(std::move(**kv_region));

    // Repeat the transformed child work, never the original observed graph.
    if (source.model_sequence) {
        const auto & layers = source.sequences[*source.model_sequence];
        std::vector<TemplateRegionReference> members;
        for (const auto member : layers.members) members.push_back({ &source, *source.model_sequence, member });
        auto sequence = source.plan_region_sequence(*source.model_sequence, members);
        if (!sequence) return std::unexpected(sequence.error());
        rewrites.push_back(std::move(*sequence));
    }

    return rewrites;
}


std::expected<std::vector<std::vector<TemplateInstance>>, TemplateGap> TemplateBank::instantiate(std::span<const InvokeStep> invocations,
                                                                                                 std::uint32_t request_capacity) const {
    const auto & first = invocations.front();
    // Source identity, capacities and initialization were qualified once by
    // configuration selection. Only target work changes between invocations.

    // Check the entire invocation group before selecting or assembling any
    // subgraph. Exact source-shape matches must obey this capacity as well.
    for (const auto & invocation : invocations) {
        for (const auto & request : invocation.input->requests) {
            const auto sequence = static_cast<std::uint64_t>(request.context_tokens) + request.input_tokens;
            if (sequence > *source_.sequence_capacity_tokens)
                return std::unexpected(TemplateGap{ TemplateGapKind::Rule, "sequence_capacity_exceeded", invocation.step, invocation.input->phase, 0 });
        }
    }

    std::optional<std::pair<std::tuple<std::size_t, std::uint32_t, MatchStage>, TemplateGap>> failure;
    struct LocalCohort {
        const TemplateSource * source;
        std::optional<std::uint64_t> batch;
    };
    struct RoleCandidates {
        std::vector<LocalCohort> cohorts;
        std::size_t cursor = 0;
        std::vector<TemplateInstance> selected;
    };
    std::vector<RoleCandidates> roles(source_.rank_count());
    for (std::uint32_t rank = 0; rank < source_.rank_count(); ++rank) {
        const auto role = source_.rank_bindings[rank].source_rank;
        for (const auto * bank : banks_) {
            const auto & anchor_source = bank->source_;
            if (anchor_source.source_rank != role) continue;
            if (anchor_source.workspace_binding_gap) {
                if (!failure)
                    failure = std::pair{
                        std::tuple(std::size_t{ 0 },
                        rank, MatchStage::Source),
                        TemplateGap{ TemplateGapKind::Observation, *anchor_source.workspace_binding_gap, first.step, first.input->phase, rank }
                    };
                continue;
            }
            const auto found = anchor_source.definitions.find({ first.step, first.input->phase, role });
            if (found == anchor_source.definitions.end()) continue;

            // Independent sessions have unrelated native resource IDs. A role
            // source must export common roles before it can be substituted.
            if (&anchor_source != &source_ && std::ranges::any_of(anchor_source.definitions, [](const auto & entry) {
                    return std::ranges::any_of(entry.second, [](const auto & definition) { return !definition.has_resource_roles(); });
                })) {
                if (!failure)
                    failure = std::pair{
                        std::tuple(std::size_t{ 0 },
                        rank, MatchStage::Source),
                        TemplateGap{ TemplateGapKind::Rule, "cross_source_resource_role_not_qualified", first.step, first.input->phase, rank }
                    };
                continue;
            }

            std::set<std::optional<std::uint64_t>> attempted;
            for (const auto & anchor : found->second)
                if (attempted.insert(anchor.source_batch).second) roles[rank].cohorts.push_back({ &anchor_source, anchor.source_batch });
        }
        if (!roles[rank].cohorts.empty()) {
            failure.reset();
            continue;
        }
        if (failure) return std::unexpected(failure->second);
        return std::unexpected(TemplateGap{ TemplateGapKind::Observation, "native_worker_role_not_observed", first.step, first.input->phase, rank });
    }

    // Materialize one local cohort per rank, lazily trying alternatives. Each
    // role retains its complete native lifecycle; source batch IDs are local.
    std::size_t rank = 0;
    while (true) {
        while (rank < roles.size()) {
            auto & role = roles[rank];
            if (role.cursor == role.cohorts.size()) {
                role.cursor = 0;
                if (rank == 0) return std::unexpected(failure->second);
                --rank;
                ++roles[rank].cursor;
                roles[rank].selected.clear();
                continue;
            }
            if (role.selected.empty()) {
                const auto & cohort = role.cohorts[role.cursor];
                for (std::size_t index = 0; index < invocations.size(); ++index) {
                    auto instance = instantiate_step(*cohort.source, invocations[index], cohort.batch, static_cast<std::uint32_t>(rank), request_capacity);
                    if (!instance) {
                        const auto progress = std::tuple(index, static_cast<std::uint32_t>(rank), instance.error().stage);
                        if (!failure || progress > failure->first) failure = std::pair(progress, instance.error().gap);
                        break;
                    }
                    const auto bound = instance->bind_workspace_uses(*invocations[index].input);
                    if (!bound) {
                        const auto progress = std::tuple(index, static_cast<std::uint32_t>(rank), MatchStage::Structure);
                        if (!failure || progress > failure->first)
                            failure = std::pair(progress,
                                                TemplateGap{ TemplateGapKind::Rule,
                                                             bound.error(),
                                                             invocations[index].step,
                                                             invocations[index].input->phase,
                                                             static_cast<std::uint32_t>(rank) });
                        break;
                    }
                    role.selected.push_back(std::move(*instance));
                }
                if (role.selected.size() != invocations.size()) {
                    role.selected.clear();
                    ++role.cursor;
                    continue;
                }
            }
            ++rank;
        }

        // Joint rewriting borrows immutable local plans. A rejected interface
        // combination must not mutate the candidates used by the next attempt.
        std::vector<std::vector<TemplateInstance>> selected(invocations.size());
        for (std::size_t index = 0; index < invocations.size(); ++index)
            for (const auto & role : roles) selected[index].push_back(role.selected[index]);

        bool qualified = true;
        for (std::size_t index = 0; index < invocations.size(); ++index) {
            const auto communication = parameterize_communications(selected[index], invocations[index]);
            if (communication) continue;
            const auto progress = std::tuple(index, source_.rank_count(), MatchStage::Communication);
            if (!failure || progress > failure->first) failure = std::pair(progress, communication.error());
            qualified = false;
            break;
        }
        if (qualified) return selected;

        --rank;
        ++roles[rank].cursor;
        roles[rank].selected.clear();
    }
}

std::expected<void, TemplateBank::SelectionFailure> TemplateBank::qualify_source(const TemplateDefinition & source, const InvokeStep & invocation) {
    const auto & input = *invocation.input;
    const auto rank = source.rank;
    if (invocation.step == StepKind::Schedule) {
        if (!source.input_reception)
            return std::unexpected(SelectionFailure{ MatchStage::Input, TemplateGapKind::Observation, source.input_reception.error(), invocation, rank });
        if (!*source.input_reception)
            return std::unexpected(SelectionFailure{ MatchStage::Input, TemplateGapKind::Rule, "native_input_request_path_not_supported", invocation, rank });
    }
    const bool same_count = source.source_requests.size() == input.requests.size();
    if (!same_count) {
        // Count changes need both parameterized device work and movable request
        // loops. The rewrite planner still qualifies their full interfaces.
        if (!source.length_domain || (input.phase == BatchPhase::Prefill && invocation.step == StepKind::Schedule && !source.admission_sequence)
            || (input.phase == BatchPhase::Prefill && invocation.step == StepKind::Prepare && !source.request_initialization_sequence)
            || (invocation.step == StepKind::Publish && (!source.publication_sequence || !source.publication_layout)))
            return std::unexpected(
                SelectionFailure{ MatchStage::RequestCount, TemplateGapKind::Rule, "request_count_transform_not_implemented", invocation, rank });
    }

    if (invocation.step == StepKind::Publish && same_count) {
        for (std::size_t slot = 0; slot < input.requests.size(); ++slot) {
            const auto finishes = source.publication_finishes[slot];
            if (!finishes)
                return std::unexpected(
                    SelectionFailure{ MatchStage::Publication, TemplateGapKind::Observation, "native_publication_branch_not_observed", invocation, rank });
            if (*finishes != input.requests[slot].finishes && !source.publication_sequence)
                return std::unexpected(
                    SelectionFailure{ MatchStage::Publication, TemplateGapKind::Rule, "publication_member_branch_rewrite_not_implemented", invocation, rank });
        }
    }

    const bool same_shapes = std::ranges::equal(source.source_requests, input.requests, [](const SourceRequest & source, const RequestSlice & target) {
        return source.input_tokens == target.input_tokens && source.context_tokens == target.context_tokens;
    });
    if (!same_shapes && (!source.length_domain || !source.length_domain->accepts(input)))
        return std::unexpected(SelectionFailure{ MatchStage::Shape,
                                                 TemplateGapKind::Rule,
                                                 source.length_domain ? "request_length_outside_parameter_domain" : "internal_shape_transform_not_implemented",
                                                 invocation,
                                                 rank });

    if (invocation.step == StepKind::Publish && same_count) {
        for (std::size_t slot = 0; slot < input.requests.size(); ++slot) {
            const auto observed = source.source_requests[slot].generated_tokens;
            const auto & target = input.requests[slot];
            if (observed == target.generated_tokens) continue;
            if (!source.output_policy)
                return std::unexpected(
                    SelectionFailure{ MatchStage::Structure, TemplateGapKind::Observation, "publication_output_policy_missing", invocation, rank });

            const auto before = source.output_policy->evaluate(observed, target.finishes);
            const auto after = source.output_policy->evaluate(target.generated_tokens, target.finishes);
            if (source.publication_cpu_progress && !after.emits && before == after) continue;
            if (source.publication_layout) {
                // This contract qualifies every target member at once.
                const auto output = source.publication_layout->qualify(input);
                if (output) return {};
                return std::unexpected(SelectionFailure{ MatchStage::Structure, TemplateGapKind::Rule, output.error(), invocation, rank });
            }
            return std::unexpected(
                SelectionFailure{ MatchStage::Structure,
                                  TemplateGapKind::Rule,
                                  before == after ? "publication_shared_work_transform_not_implemented" : "publication_output_branch_rewrite_not_implemented",
                                  invocation,
                                  rank });
        }
    }
    if (invocation.step != StepKind::Schedule) return {};

    if (!source.transition)
        return std::unexpected(
            SelectionFailure{ MatchStage::Structure, TemplateGapKind::Observation, "batch_transition_state_not_observed", invocation, rank });
    if (*source.transition != input.transition && !source.transition_region)
        return std::unexpected(
            SelectionFailure{ MatchStage::Structure, TemplateGapKind::Rule, "batch_transition_prefix_rewrite_not_implemented", invocation, rank });
    if (source.transition_region && !source.running_update_region) {
        const auto source_prefix = source.transition->pending_prefill_requests ? 1U : 0U;
        const auto target_prefix = input.transition.pending_prefill_requests ? 1U : 0U;
        if (!std::ranges::equal(std::span(source.filters).subspan(source_prefix), std::span(input.filters).subspan(target_prefix)))
            return std::unexpected(
                SelectionFailure{ MatchStage::Structure, TemplateGapKind::Rule, "batch_update_filter_transform_not_implemented", invocation, rank });
    }
    if (!source.admission)
        return std::unexpected(SelectionFailure{ MatchStage::Structure, TemplateGapKind::Observation, "admission_entry_state_not_observed", invocation, rank });

    // Decode carries running members but admits no new prefill requests.
    const auto admitted = input.phase == BatchPhase::Prefill ? input.requests.size() : 0;
    const bool parameterized = source.admission_cpu_request_count
                               && (*source.admission_cpu_request_count == admitted || (admitted > 0 && source.admission_sequence))
                               && input.admission_work.parameterized_cpu_path(input.admission, admitted);
    if (*source.admission != input.admission && !parameterized)
        return std::unexpected(
            SelectionFailure{ MatchStage::Structure, TemplateGapKind::Rule, "admission_prefix_work_transform_not_implemented", invocation, rank });
    if (!source.admission_work)
        return std::unexpected(SelectionFailure{ MatchStage::Structure, TemplateGapKind::Observation, "admission_work_not_qualified", invocation, rank });
    if (*source.admission_work != input.admission_work && !parameterized)
        return std::unexpected(SelectionFailure{ MatchStage::Structure, TemplateGapKind::Rule, "admission_work_transform_not_implemented", invocation, rank });

    if (!source.transition_region) {
        if (source.filters != input.filters)
            return std::unexpected(SelectionFailure{ MatchStage::Structure,
                                                     TemplateGapKind::Rule,
                                                     source.filters.size() == input.filters.size() ? "batch_filter_membership_transform_not_implemented"
                                                                                                   : "batch_filter_sequence_rewrite_not_implemented",
                                                     invocation,
                                                     rank });
        if (source.merge != input.merge)
            return std::unexpected(
                SelectionFailure{ MatchStage::Structure, TemplateGapKind::Rule, "batch_merge_branch_rewrite_not_implemented", invocation, rank });
    }
    return {};
}

std::expected<TemplateInstance, TemplateBank::SelectionFailure> TemplateBank::instantiate_step(const TemplateSource & anchor_source,
                                                                                               const InvokeStep & invocation,
                                                                                               std::optional<std::uint64_t> source_batch, std::uint32_t rank,
                                                                                               std::uint32_t request_capacity) const {
    const auto & binding = anchor_source.rank_bindings.at(rank);
    const auto found = anchor_source.definitions.find({ invocation.step, invocation.input->phase, binding.source_rank });
    if (found == anchor_source.definitions.end())
        return std::unexpected(SelectionFailure{ MatchStage::Source, TemplateGapKind::Observation, "native_step_not_observed", invocation, rank });

    SelectionFailure failure{ MatchStage::Source, TemplateGapKind::Observation, "native_batch_step_not_observed", invocation, rank };
    for (const auto & source : found->second) {
        // Anchor the model lifecycle to one local cohort; qualified input and
        // member cuts are composed separately before the roles are paired.
        if (source.source_batch != source_batch) continue;
        auto qualified = qualify_source(source, invocation);
        if (qualified && invocation.step == StepKind::Publish && !invocation.input->requests.empty() && binding.publishes_outputs
            && (!source.output_policy || !source.output_boundary))
            qualified = std::unexpected(
                SelectionFailure{ MatchStage::Publication, TemplateGapKind::Observation, "request_output_boundary_not_observed", invocation, rank });
        if (qualified) {
            auto candidate = instantiate_source(source, invocation, request_capacity, rank, binding);
            if (candidate) return std::move(*candidate);
            qualified = std::unexpected(SelectionFailure{ MatchStage::Structure, TemplateGapKind::Rule, candidate.error(), invocation, rank });
        }
        auto rejected = qualified.error();
        rejected.gap.rank = rank;
        if (rejected.stage > failure.stage || (rejected.stage == failure.stage && rejected.gap.kind > failure.gap.kind)) failure = rejected;
    }
    return std::unexpected(std::move(failure));
}

std::expected<TemplateInstance, std::string> TemplateBank::instantiate_source(const TemplateDefinition & source, const InvokeStep & invocation,
                                                                              std::uint32_t request_capacity, std::uint32_t target_rank,
                                                                              const TemplateRankBinding & rank_binding) const {
    TemplateInstance instance(invocation.work_id, source, target_rank, rank_binding);
    const auto reception = bind_input_reception(instance.topology, source, *invocation.input);
    if (!reception) return std::unexpected(reception.error());

    const auto rewrites = plan_materializations(source, *invocation.input);
    if (!rewrites) return std::unexpected(rewrites.error());
    const auto applied = instance.topology.apply_operations(source, *rewrites);
    if (!applied) return std::unexpected(applied.error());

    for (const auto index : instance.topology.nodes()) {
        const auto & node = source.graph[index];
        const auto & node_observation = source.observation(node);
        const auto transfer = source.transfer_layouts.find(node_observation.source_span);
        if (node_observation.transfer && transfer != source.transfer_layouts.end()) {
            auto target = transfer->second.evaluate(*node_observation.transfer, *invocation.input);
            if (!target) return std::unexpected(target.error());
            instance.transfer_overrides.emplace(node_observation.source_span, *target);
        }

        if (!node_observation.kernel) continue;
        auto target = transform_kernel(node, source, invocation, request_capacity);
        if (!target) return std::unexpected(target.error());
        if (!*target) continue;

        // Compiled model rules already carry identity relations; only
        // materialized descriptor changes need an extra override record.
        const auto & kernel = **target;
        if (source.graph.model_rule(node.observation_slot) && kernel.input_shapes == node_observation.kernel->input_shapes
            && kernel.output_shapes == node_observation.kernel->output_shapes && !kernel.logits_token_indices && !kernel.positions
            && kernel.value_dependencies.empty())
            continue;
        instance.kernel_overrides.emplace(node_observation.source_span, std::move(**target));
    }

    if (source.publication_layout) {
        auto output = source.publication_layout->qualify(*invocation.input);
        if (!output) return std::unexpected(output.error());
        const bool retained =
            std::ranges::any_of(instance.topology.nodes(), [&](std::size_t index) { return source.observation(source.graph[index]).source_span == *output; });
        if (!retained) return std::unexpected("publication_output_region_replacement_not_qualified");
    }

    const auto inserted = parameterize_insertions(instance.topology, invocation, request_capacity);
    if (!inserted) return std::unexpected(inserted.error());
    return instance;
}

std::expected<std::optional<SourceKernel>, std::string> TemplateBank::transform_kernel(const TemplateNode & node, const TemplateDefinition & source,
                                                                                       const InvokeStep & invocation, std::uint32_t request_capacity,
                                                                                       std::optional<std::size_t> request_slot,
                                                                                       std::optional<BatchFilterBinding> filter_binding) const {
    // Production binds component semantics before generic shape matching.
    // Inserted nodes use their donor's binding with this instance's target state.
    const auto & observation = source.observation(node);
    const auto kv = source.kv_kernel_layouts.find(observation.source_span);
    if (kv != source.kv_kernel_layouts.end()) {
        if (!kv->second) return std::unexpected(kv->second.error());
        auto target = kv->second->evaluate(*observation.kernel, *invocation.input, request_slot);
        if (!target) return std::unexpected(target.error());
        return std::move(*target);
    }

    const auto batch = source.batch_kernel_layouts.find(observation.source_span);
    if (batch != source.batch_kernel_layouts.end()) {
        if (!batch->second) return std::unexpected(batch->second.error());
        auto target = filter_binding ? batch->second->with_filter_binding(*filter_binding).evaluate(*observation.kernel, *invocation.input, request_slot)
                                     : batch->second->evaluate(*observation.kernel, *invocation.input, request_slot);
        if (!target) return std::unexpected(target.error());
        return std::move(*target);
    }

    const auto attention = source.attention_kernel_layouts.find(observation.source_span);
    if (attention != source.attention_kernel_layouts.end()) return attention->second->evaluate(*observation.kernel, *invocation.input);
    if (const auto * model = source.graph.model_rule(node.observation_slot)) return model->evaluate(*observation.kernel, *invocation.input);

    const auto pool = source.request_pool_layouts.find(observation.source_span);
    if (pool != source.request_pool_layouts.end()) {
        if (!pool->second) return std::unexpected(pool->second.error());
        auto target = pool->second->evaluate(*observation.kernel, *invocation.input, request_capacity);
        if (!target) return std::unexpected(target.error());
        return std::move(*target);
    }
    return std::optional<SourceKernel>{};
}

std::expected<void, std::string> TemplateBank::parameterize_insertions(TemplateTopology & topology, const InvokeStep & invocation,
                                                                       std::uint32_t request_capacity) const {
    for (std::size_t index = 0; index < topology.inserted_nodes().size(); ++index) {
        const auto & inserted = topology.inserted_nodes()[index];
        if (!inserted.observation().kernel) {
            if (inserted.observation().transfer) {
                // Synchronous driver copies retain their transfer cost. Their
                // parameters must follow relocation just like device DMA.
                const auto relation = inserted.definition->transfer_layouts.find(inserted.observation().source_span);
                if (relation != inserted.definition->transfer_layouts.end()) {
                    auto layout = relation->second;
                    if (inserted.filter_binding)
                        layout.extent = *inserted.filter_binding == BatchFilterBinding::Transition ? TransferExtent::TransitionRetainedRequests
                                                                                                   : TransferExtent::RunningRetainedRequests;
                    auto target = layout.evaluate(*inserted.observation().transfer, *invocation.input);
                    if (!target) return std::unexpected(target.error());
                    topology.bind_inserted_transfer(index, *target);
                }
                else return std::unexpected("inserted_transfer_parameter_relation_missing");
            }
            else if (inserted.node.kind == SourceSpanKind::Dma) return std::unexpected("inserted_transfer_observation_missing");
            if (inserted.node.kind == SourceSpanKind::Compute) return std::unexpected("inserted_kernel_observation_missing");
            continue;
        }
        auto target = transform_kernel(inserted.node, *inserted.definition, invocation, request_capacity, inserted.request_slot, inserted.filter_binding);
        if (!target) return std::unexpected(target.error());
        if (!*target) return std::unexpected("inserted_kernel_parameter_relation_missing");
        topology.bind_inserted_kernel(index, std::move(**target));
    }
    return {};
}

std::uint32_t TemplateBank::page_size_tokens() const { return source_.page_size_tokens; }

const std::expected<AdmissionPolicy, TemplateGap> & TemplateBank::admission_policy() const { return source_.admission_policy; }

const TemplateWorkspacePool & TemplateBank::workspace_pool(const TemplateDefinition & definition, std::size_t index) const {
    const auto bank =
        std::ranges::find_if(banks_, [&definition](const auto * candidate) { return candidate->source_.manifest_path == definition.source_manifest; });
    return (*bank)->source_.workspace_pools.at(index);
}

std::expected<TemplateOperationRewrite, std::string> TemplateBank::plan_workspace_replacement(const TemplateInstance & instance,
                                                                                              const TemplateWorkspaceUseInstance & use, WorkspaceBranch branch,
                                                                                              const BatchInput & input) const {
    const auto & source = *use.definition;
    const auto & observed = source.workspace_uses[use.source_use];
    if (&source != instance.definition || use.begin != observed.begin || use.end != observed.end)
        return std::unexpected("inserted_workspace_replacement_not_qualified");
    const auto cut = source.workspace_region(observed);
    for (const auto node : cut.nodes)
        if (!std::ranges::binary_search(instance.topology.nodes(), node)) return std::unexpected("workspace_replacement_overlaps_prior_rewrite");

    const auto & pool = workspace_pool(source, observed.pool);
    std::string failure = "workspace_branch_donor_missing";
    for (const auto * donor : donors_.at({ source.step, source.phase, source.rank })) {
        for (const auto & candidate : donor->workspace_uses) {
            if (candidate.branch != branch || donor->operations[candidate.operation].name != source.operations[observed.operation].name) continue;
            const auto & donor_pool = workspace_pool(*donor, candidate.pool);
            if (donor_pool.resource != pool.resource || donor_pool.allocation_padding_bytes != pool.allocation_padding_bytes
                || donor_pool.allocation_alignment_bytes != pool.allocation_alignment_bytes)
                continue;
            if (use.demand && !candidate.demand) {
                failure = candidate.demand.error();
                continue;
            }
            if (use.demand) {
                const auto demand = (*candidate.demand)->evaluate(input);
                if (!demand || demand->bytes != use.demand->bytes || demand->work_units != use.demand->work_units) {
                    failure = "workspace_donor_demand_not_equivalent";
                    continue;
                }
            }

            auto replacement = source.plan_workspace_replacement(observed, *donor, candidate);
            if (replacement) return replacement;
            failure = replacement.error();
        }
    }
    return std::unexpected(failure);
}

std::expected<void, TemplateGap> TemplateBank::qualify(std::uint32_t request_capacity) const {
    if (source_.source_gap) return std::unexpected(*source_.source_gap);
    if (!source_.admission_policy) return std::unexpected(source_.admission_policy.error());
    if (!source_.sequence_capacity_tokens)
        return std::unexpected(TemplateGap{ TemplateGapKind::Observation, "sequence_capacity_not_observed", StepKind::Schedule, BatchPhase::Idle, 0 });
    if (const auto gap = source_.request_pool_domain.unsupported(request_capacity))
        return std::unexpected(TemplateGap{ TemplateGapKind::Rule, *gap, StepKind::Schedule, BatchPhase::Idle, 0 });
    return {};
}

std::expected<void, TemplateGap> TemplateBank::qualify_initialization(std::uint32_t request_capacity) const {
    if (source_.steady_state_gap)
        return std::unexpected(TemplateGap{ TemplateGapKind::Observation, *source_.steady_state_gap, StepKind::Schedule, BatchPhase::Idle, 0 });
    if (source_.workspace_binding_gap)
        return std::unexpected(TemplateGap{ TemplateGapKind::Observation, *source_.workspace_binding_gap, StepKind::Schedule, BatchPhase::Idle, 0 });
    if (const auto gap = source_.initialization_pool_domain.unsupported(request_capacity))
        return std::unexpected(TemplateGap{ TemplateGapKind::Rule, *gap, StepKind::Schedule, BatchPhase::Idle, 0 });
    return {};
}

nlohmann::json TemplateBank::describe() const {
    std::size_t workspace_uses = 0;
    std::size_t workspace_synchronizations = 0;
    std::size_t workspace_demand_rules = 0;
    std::map<std::string, std::size_t> workspace_demand_gaps;
    for (const auto & [key, definitions] : source_.definitions) {
        for (const auto & definition : definitions) {
            workspace_uses += definition.workspace_uses.size();
            for (const auto & use : definition.workspace_uses) {
                if (use.requires_device_synchronization) ++workspace_synchronizations;
                if (use.demand) ++workspace_demand_rules;
                else if (use.branch != WorkspaceBranch::Release) ++workspace_demand_gaps[use.demand.error()];
            }
        }
    }
    auto workspace_pools = Json::array();
    for (const auto & pool : source_.workspace_pools)
        workspace_pools.push_back({
            {               "resource",    pool.resource.describe() },
            { "initial_capacity_bytes", pool.initial_capacity_bytes }
        });


    std::size_t source_instances = 0;
    std::size_t shared_regions = 0;
    std::size_t regions_with_cut = 0;
    std::size_t source_sequences = 0;
    std::size_t model_rule_observations = 0;
    std::size_t attention_rule_observations = 0;
    std::set<std::string> observed_semantics;
    std::map<std::string, std::size_t> structure_kinds;
    std::map<std::string, std::size_t> fragment_kinds;
    std::map<SourceSpanKind, std::size_t> definition_nodes;
    std::map<SourceSpanKind, std::size_t> source_nodes;
    std::set<TemplateResource> resources;
    for (const auto & [key, instances] : source_.definitions) {
        source_instances += instances.size();

        // Each row is a source call. A step/phase/rank bucket is not a
        // structural family, and repeated observations are not new variants.
        for (const auto & source : instances) {
            observed_semantics.insert("serving." + std::string(ServingContract{}.block(source.step).name));
            for (const auto & region : source.regions) observed_semantics.insert(region.semantic);
            for (const auto & node : source.graph) {
                ++source_nodes[node.kind];
                if (source.graph.model_rule(node.observation_slot)) ++model_rule_observations;
            }
            for (const auto & lane : source.lanes)
                if (lane.source.domain == SourceDomain::CpuThread || lane.source.domain == SourceDomain::DeviceStream) resources.insert(lane.resource());
            for (const auto & wait : source.stream_waits) resources.insert(wait.resources.begin(), wait.resources.end());
            for (const auto & connection : source.connection_observations)
                if (connection.resource) resources.insert(*connection.resource);

            shared_regions += source.graph.shared_regions();
            regions_with_cut += std::ranges::count_if(source.regions, [](const auto & region) { return !region.boundaries.empty(); });
            source_sequences += source.sequences.size();
            attention_rule_observations += source.attention_kernel_layouts.size();
        }
    }

    // Anonymous sequence fragments share storage without declaring new flow semantics.
    for (const auto & structure : source_.region_structures) {
        auto & kinds = observed_semantics.contains(structure.semantic) ? structure_kinds : fragment_kinds;
        ++kinds[structure.semantic];
        for (const auto & node : structure.nodes) ++definition_nodes[node.kind];
    }

    auto source_node_counts = Json::object();
    auto definition_node_counts = Json::object();
    for (const auto & [kind, count] : source_nodes) source_node_counts[std::string(SourceGraph::span_name(kind))] = count;
    for (const auto & [kind, count] : definition_nodes) definition_node_counts[std::string(SourceGraph::span_name(kind))] = count;

    Json admission;
    if (source_.admission_policy) {
        const auto & policy = *source_.admission_policy;
        admission = {
            {                       "status",                         "available" },
            {                "initial_ratio",                policy.initial_ratio },
            {                "minimum_ratio",                policy.minimum_ratio },
            {             "decay_per_decode",             policy.decay_per_decode },
            { "output_estimate_limit_tokens", policy.output_estimate_limit_tokens },
            {   "reserve_tokens_per_request",   policy.reserve_tokens_per_request }
        };
    }
    else {
        const auto & gap = source_.admission_policy.error();
        admission = {
            { "status", gap.kind == TemplateGapKind::Observation ? "needs_data" : "unsupported" },
            { "reason",                                                              gap.reason }
        };
    }

    const auto qualification = qualify(source_.request_pool_domain.maximum);
    Json source_qualification{
        {  "scope", "source_identity_policy_and_capacities" },
        { "status",                             "available" }
    };
    if (!qualification) {
        const auto & gap = qualification.error();
        source_qualification["status"] = gap.kind == TemplateGapKind::Observation ? "needs_data" : "unsupported";
        source_qualification["reason"] = gap.reason;
    }

    const auto initialization = qualify_initialization(source_.request_pool_domain.maximum);
    Json initialization_status{
        { "status", "available" }
    };
    if (!initialization) {
        const auto & gap = initialization.error();
        initialization_status["status"] = gap.kind == TemplateGapKind::Observation ? "needs_data" : "unsupported";
        initialization_status["reason"] = gap.reason;
    }

    std::map<std::string, std::map<std::string, std::size_t>> source_gaps;
    for (const auto & gap : source_.gaps) ++source_gaps[gap.at("kind").get<std::string>()][gap.at("reason").get<std::string>()];

    Json result{
        {                  "initialization",                                      initialization_status                                            },
        {                       "workspace",
         { { "source_binding", source_.workspace_binding_gap ? "needs_data" : "available" },
         { "binding_gap", source_.workspace_binding_gap ? Json(*source_.workspace_binding_gap) : Json(nullptr) },
         { "pools", workspace_pools },
         { "operation_uses", workspace_uses },
         { "demand_rules", workspace_demand_rules },
         { "demand_rule_definitions", source_.workspace_rules.size() },
         { "demand_gaps", workspace_demand_gaps },
         { "required_device_synchronizations", workspace_synchronizations } }                                                                      },
        {        "sequence_capacity_tokens",             source_.sequence_capacity_tokens ? Json(*source_.sequence_capacity_tokens) : Json(nullptr) },
        {            "static_qualification",                                                                                   source_qualification },
        {             "request_pool_domain",
         { { "maximum", source_.request_pool_domain.maximum },
         { "shrink_gap", source_.request_pool_domain.shrink_gap ? Json(*source_.request_pool_domain.shrink_gap) : Json(nullptr) } }                },
        {                "admission_policy",                                                                                              admission },
        {                        "manifest",                                                                         source_.manifest_path.string() },
        {                     "source_rank",                                                                                    source_.source_rank },
        {                      "resolution",                                                                                     source_.resolution },
        {                "page_size_tokens",                                                                               source_.page_size_tokens },
        { "observed_step_phase_rank_groups",                                                                             source_.definitions.size() },
        {                "source_instances",                                                                                       source_instances },
        {        "region_structure_sharing",
         { { "definitions", source_.region_structures.size() },
         { "regions", shared_regions },
         { "regions_with_cut", regions_with_cut },
         { "source_sequences", source_sequences },
         { "semantic_kinds", structure_kinds.size() },
         { "definitions_by_semantic", structure_kinds },
         { "fragment_kinds", fragment_kinds.size() },
         { "fragment_definitions_by_label", fragment_kinds },
         { "source_nodes_by_kind", source_node_counts },
         { "definition_nodes_by_kind", definition_node_counts },
         { "scope", "semantic_self_work_rules_and_local_resource_aliases" },
         { "composition", "source_region_tree_and_target_bindings" } }                                                                             },
        {       "attention_parameter_rules", { { "definitions", source_.attention_rules.size() }, { "observations", attention_rule_observations } } },
        {           "model_parameter_rules",         { { "definitions", source_.model_rules.size() }, { "observations", model_rule_observations } } },
        {          "resource_binding_count",                                                                                       resources.size() },
        {              "source_gap_summary",                          { { "records", source_.gaps.size() }, { "by_kind_and_reason", source_gaps } } },
        {                      "transforms",                                                                                     source_.transforms },
        {                          "limits",                                                                                         source_.limits }
    };
    return result;
}

} // namespace markov
