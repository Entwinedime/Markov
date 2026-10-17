#include "markov/templates/templates.hpp"

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

void TemplateBank::bind_sources(std::span<const TemplateBank * const> banks, std::span<const TemplateDefinition * const> operation_sources) {
    operation_sources_.assign(operation_sources.begin(), operation_sources.end());
    donors_.clear();
    banks_ = { this };
    for (const auto & [key, definitions] : source_.definitions)
        for (const auto & definition : definitions) donors_[key].push_back(&definition);

    for (const auto * bank : banks) {
        if (bank == this) continue;
        const auto & source = bank->source_;
        // Local rewrites may share observations under the same native setup.
        // Configuration migration remains a separate, explicitly qualified rule.
        if (source.model_path != source_.model_path || source.configuration != source_.configuration || source.ranks != source_.ranks
            || source.page_size_tokens != source_.page_size_tokens || source.sequence_capacity_tokens != source_.sequence_capacity_tokens
            || *source.admission_policy != *source_.admission_policy || source.resolution != source_.resolution
            || source.deployment.service != source_.deployment.service || source.deployment.devices != source_.deployment.devices)
            continue;

        banks_.push_back(bank);
        for (const auto & [key, definitions] : source.definitions)
            for (const auto & definition : definitions) donors_[key].push_back(&definition);
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
            const auto cut = source.semantic_region(source.regions[*source.running_update_region]);
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
    if (source.publication_regions.empty()) return rewrites;
    auto work = source.publication_layout->evaluate(target);
    if (!work) return std::unexpected(work.error());

    // Reuse qualified member cuts across calls of the same phase/rank and
    // policy. Shared output and aggregate release remain with the recipient.
    std::vector<const TemplateDefinition *> donors{ &source };
    for (const auto * candidate : donors_.at({ source.step, source.phase, source.rank })) {
        if (candidate != &source && !candidate->publication_regions.empty()
            && source.publication_layout->can_reuse_members_from(*candidate->publication_layout))
            donors.push_back(candidate);
    }

    bool changed = source.publication_regions.size() != target.requests.size();
    std::vector<TemplateRegionReference> members;
    for (std::size_t slot = 0; slot < target.requests.size(); ++slot) {
        const auto finishes = target.requests[slot].finishes;
        if (slot < source.publication_regions.size() && source.publication_finishes[slot] == finishes) {
            members.push_back({ &source, source.publication_regions[slot] });
            continue;
        }

        bool selected = false;
        std::string gap = "publication_member_branch_not_qualified";
        for (const auto * donor : donors) {
            for (std::size_t donor_slot = 0; donor_slot < donor->publication_regions.size(); ++donor_slot) {
                if (donor->publication_finishes[donor_slot] != std::optional(finishes)) continue;
                // A member is selected by its own interface. Its predecessor
                // belongs to the assembled sequence, not the old recipient slot.
                const auto ports = donor->region_ports(donor->semantic_region(donor->regions[donor->publication_regions[donor_slot]]));
                if (!ports) {
                    gap = ports.error();
                    continue;
                }

                members.push_back({ donor, donor->publication_regions[donor_slot] });
                selected = true;
                break;
            }
            if (selected) break;
        }
        if (!selected) return std::unexpected(gap);

        changed = true;
    }
    if (changed) {
        auto sequence = source.plan_region_sequence(source.publication_regions, members);
        if (!sequence) return std::unexpected(sequence.error());
        rewrites.push_back(std::move(*sequence));
    }
    return rewrites;
}

std::expected<std::vector<TemplateOperationRewrite>, std::string> TemplateBank::plan_materializations(const TemplateDefinition & source,
                                                                                                      const BatchInput & target) const {
    auto kv_region = plan_kv_region(source, target);
    if (!kv_region) return std::unexpected(kv_region.error());

    auto operations = plan_batch_operations(source, target);
    if (!operations) return std::unexpected(operations.error());
    auto rewrites = std::move(*operations);

    if (source.source_requests.size() != target.requests.size()) {
        const bool admission = source.step == StepKind::Schedule && source.phase == BatchPhase::Prefill;
        const auto & regions = admission ? source.admission_regions : source.request_initialization_regions;
        if (admission) {
            if (regions.empty() || !target.admission_work.parameterized_cpu_path(target.admission, target.requests.size()))
                return std::unexpected("admission_member_sequence_not_qualified");
            // The first admission member initializes native state; only an
            // observed later member supplies the insertion branch for extra slots.
            if (target.requests.size() > 1 && regions.size() < 2) return std::unexpected("admission_insertion_member_not_observed");
        }
        if (!regions.empty()) {
            std::vector<TemplateRegionReference> members;
            for (std::size_t slot = 0; slot < target.requests.size(); ++slot) {
                const auto index = regions[std::min(slot, regions.size() - 1)];
                members.push_back({ &source, index });
            }
            auto sequence = source.plan_region_sequence(regions, members);
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

    return rewrites;
}


std::expected<std::vector<std::vector<TemplateInstance>>, TemplateGap> TemplateBank::instantiate(std::span<const InvokeStep> invocations,
                                                                                                 std::uint32_t request_capacity) const {
    const auto & first = invocations.front();
    auto qualification = qualify(request_capacity);
    if (qualification) qualification = qualify_initialization(request_capacity);
    if (!qualification) {
        auto gap = qualification.error();
        gap.step = first.step;
        gap.phase = first.input.phase;
        return std::unexpected(std::move(gap));
    }

    // Check the entire invocation group before selecting or assembling any
    // subgraph. Exact source-shape matches must obey this capacity as well.
    for (const auto & invocation : invocations) {
        for (const auto & request : invocation.input.requests) {
            const auto sequence = static_cast<std::uint64_t>(request.context_tokens) + request.input_tokens;
            if (sequence > *source_.sequence_capacity_tokens)
                return std::unexpected(TemplateGap{ TemplateGapKind::Rule, "sequence_capacity_exceeded", invocation.step, invocation.input.phase, 0 });
        }
    }

    std::optional<std::pair<std::tuple<std::size_t, std::uint32_t, MatchStage>, TemplateGap>> failure;
    for (const auto * bank : banks_) {
        const auto * anchor_source = &bank->source_;
        // Only the selected root initializes target resources. Later cohorts
        // execute against WorkspaceState, never their source's initial capacity.
        if (anchor_source->workspace_binding_gap) {
            if (!failure)
                failure = std::pair{
                    std::tuple(std::size_t{ 0 },
                    std::uint32_t{ 0 },
                    MatchStage::Source),
                    TemplateGap{ TemplateGapKind::Observation, *anchor_source->workspace_binding_gap, first.step, first.input.phase, 0 }
                };
            continue;
        }
        const auto found = anchor_source->definitions.find({ first.step, first.input.phase, 0 });
        if (found == anchor_source->definitions.end()) continue;
        if (found->second.empty()) {
            if (!failure)
                failure = std::pair{
                    std::tuple(std::size_t{ 0 },
                    std::uint32_t{ 0 },
                    MatchStage::Source),
                    TemplateGap{ TemplateGapKind::Rule, "native_transform_rule_missing", first.step, first.input.phase, 0 }
                };
            continue;
        }

        if (anchor_source != &source_) {
            // A whole-batch source change must preserve target resource roles,
            // including frontiers left by the preceding source's work.
            bool resources_qualified = true;
            for (const auto * candidate : { &source_, anchor_source })
                for (const auto & [key, definitions] : candidate->definitions)
                    for (const auto & definition : definitions)
                        if (!definition.has_resource_roles()) resources_qualified = false;
            if (!resources_qualified) {
                if (!failure)
                    failure = std::pair{
                        std::tuple(std::size_t{ 0 },
                        std::uint32_t{ 0 },
                        MatchStage::Source),
                        TemplateGap{ TemplateGapKind::Rule, "cross_source_resource_role_not_qualified", first.step, first.input.phase, 0 }
                    };
                continue;
            }
        }
        std::set<std::optional<std::uint64_t>> attempted;
        for (const auto & anchor : found->second) {
            if (!attempted.insert(anchor.source_batch).second) continue;

            // Native cross-rank endpoints belong to one observed batch. Qualify
            // the complete cohort before any instance reaches graph assembly.
            std::vector<std::vector<TemplateInstance>> selected(invocations.size());
            bool qualified = true;
            for (std::size_t index = 0; index < invocations.size(); ++index) {
                for (std::uint32_t rank = 0; rank < source_.ranks; ++rank) {
                    auto instance = instantiate_step(*anchor_source, invocations[index], anchor.source_batch, rank, request_capacity);
                    if (!instance) {
                        // Preserve the rejection reached furthest through the
                        // cohort; an incompatible branch must not hide a rule gap.
                        const auto progress = std::tuple(index, rank, instance.error().stage);
                        if (!failure || progress > failure->first) failure = std::pair(progress, instance.error().gap);
                        qualified = false;
                        break;
                    }
                    selected[index].push_back(std::move(*instance));
                }
                if (!qualified) break;

                // Communication is qualified after every rank's local topology is
                // available, while the entire cohort still belongs to this candidate.
                const auto communication = parameterize_communications(selected[index], invocations[index]);
                if (!communication) {
                    const auto progress = std::tuple(index, source_.ranks, MatchStage::Communication);
                    if (!failure || progress > failure->first) failure = std::pair(progress, communication.error());
                    qualified = false;
                    break;
                }
            }
            if (qualified) {
                // Bind resource uses after local and joint communication rewrites.
                // A donor contributes its own provenance and target endpoints.
                for (std::size_t index = 0; index < selected.size() && qualified; ++index) {
                    for (auto & instance : selected[index]) {
                        const auto bound = instance.bind_workspace_uses(invocations[index].input);
                        if (bound) continue;
                        const auto & invocation = invocations[index];
                        const auto rank = instance.definition->rank;
                        const auto progress = std::tuple(index, rank, MatchStage::Structure);
                        if (!failure || progress > failure->first)
                            failure = std::pair(progress, TemplateGap{ TemplateGapKind::Rule, bound.error(), invocation.step, invocation.input.phase, rank });
                        qualified = false;
                        break;
                    }
                }
            }
            if (qualified) return selected;
        }
    }
    if (!failure) return std::unexpected(TemplateGap{ TemplateGapKind::Observation, "native_step_not_observed", first.step, first.input.phase, 0 });
    return std::unexpected(failure->second);
}

std::expected<TemplateInstance, TemplateBank::SelectionFailure> TemplateBank::instantiate_step(const TemplateSource & anchor_source,
                                                                                               const InvokeStep & invocation,
                                                                                               std::optional<std::uint64_t> source_batch, std::uint32_t rank,
                                                                                               std::uint32_t request_capacity) const {
    const auto found = anchor_source.definitions.find({ invocation.step, invocation.input.phase, rank });
    if (found == anchor_source.definitions.end()) {
        return std::unexpected(SelectionFailure{
            MatchStage::Source,
            { TemplateGapKind::Observation, "native_step_not_observed", invocation.step, invocation.input.phase, rank }
        });
    }
    if (found->second.empty()) {
        return std::unexpected(SelectionFailure{
            MatchStage::Source,
            { TemplateGapKind::Rule, "native_transform_rule_missing", invocation.step, invocation.input.phase, rank }
        });
    }

    // Matching token shapes selects a source instance, not a proof of target equivalence.
    std::optional<TemplateInstance> selected;
    std::optional<TemplateGap> transform_gap;
    bool token_shape_match = false;
    bool length_domain_rejected = false;
    bool publication_match = false;
    bool publication_rewrite_missing = false;
    bool request_count_match = false;
    bool batch_observed = false;
    for (const auto & source : found->second) {
        // The four actions of one non-overlap batch share native source identity.
        // Independent shape matches could otherwise join unrelated iterations.
        if (source.source_batch != source_batch) continue;
        batch_observed = true;
        const bool same_count = source.source_requests.size() == invocation.input.requests.size();
        if (!same_count) {
            // A count change is only a candidate when all source device work
            // has parameter relations and each request loop has a semantic cut.
            // Materialization planning below must still qualify every operation.
            if (!source.length_domain) continue;
            if (invocation.input.phase == BatchPhase::Prefill) {
                if (invocation.step == StepKind::Schedule && source.admission_regions.empty()) continue;
                if (invocation.step == StepKind::Prepare && source.request_initialization_regions.empty()) continue;
            }
            if (invocation.step == StepKind::Publish && (source.publication_regions.empty() || !source.publication_layout)) continue;
        }

        request_count_match = true;
        bool matches = true;
        // Completion controls Publish's request loop and release work. Earlier
        // stages consume current membership and sequence inputs; future finish
        // masks must not restrict those stages or mislabel their missing rules.
        if (invocation.step == StepKind::Publish && same_count) {
            for (std::size_t slot = 0; slot < source.source_requests.size(); ++slot) {
                const auto publication = source.publication_finishes[slot];
                if (!publication) {
                    matches = false;
                    break;
                }
                if (*publication != invocation.input.requests[slot].finishes && source.publication_regions.empty()) {
                    // The source branch is known; changing it needs a region
                    // rewrite. Another observation cannot implement that rule.
                    publication_rewrite_missing = true;
                    matches = false;
                    break;
                }
            }
        }
        if (!matches) continue;
        publication_match = true;

        matches = same_count;
        if (same_count) {
            for (std::size_t slot = 0; slot < source.source_requests.size(); ++slot) {
                const auto & observed = source.source_requests[slot];
                const auto & requested = invocation.input.requests[slot];
                if (observed.input_tokens != requested.input_tokens || observed.context_tokens != requested.context_tokens) {
                    matches = false;
                    break;
                }
            }
        }
        if (!matches && source.length_domain) {
            matches = source.length_domain->accepts(invocation.input);
            length_domain_rejected = length_domain_rejected || !matches;
        }
        if (!matches) continue;
        token_shape_match = true;

        if (invocation.step == StepKind::Publish && same_count) {
            for (std::size_t slot = 0; slot < source.source_requests.size(); ++slot) {
                const auto observed = source.source_requests[slot].generated_tokens;
                const auto & target = invocation.input.requests[slot];
                if (observed == target.generated_tokens) continue;

                auto kind = TemplateGapKind::Observation;
                std::string reason = "publication_output_policy_missing";
                if (source.output_policy) {
                    kind = TemplateGapKind::Rule;
                    const auto before = source.output_policy->evaluate(observed, target.finishes);
                    const auto after = source.output_policy->evaluate(target.generated_tokens, target.finishes);
                    if (source.publication_cpu_progress && !after.emits && before == after) continue;
                    reason = before == after ? "publication_shared_work_transform_not_implemented" : "publication_output_branch_rewrite_not_implemented";
                    if (source.publication_layout) {
                        const auto output = source.publication_layout->evaluate(invocation.input);
                        if (output) continue;
                        reason = output.error();
                    }
                }
                if (!transform_gap) transform_gap = TemplateGap{ kind, std::move(reason), invocation.step, invocation.input.phase, rank };
                matches = false;
                break;
            }
        }
        if (!matches) continue;

        if (invocation.step == StepKind::Schedule) {
            if (!source.transition) {
                if (!transform_gap)
                    transform_gap =
                        TemplateGap{ TemplateGapKind::Observation, "batch_transition_state_not_observed", invocation.step, invocation.input.phase, rank };
                continue;
            }
            if (*source.transition != invocation.input.transition && !source.transition_region) {
                if (!transform_gap)
                    transform_gap =
                        TemplateGap{ TemplateGapKind::Rule, "batch_transition_prefix_rewrite_not_implemented", invocation.step, invocation.input.phase, rank };
                continue;
            }
            if (source.transition_region && !source.running_update_region) {
                const auto source_prefix = source.transition->pending_prefill_requests ? 1U : 0U;
                const auto target_prefix = invocation.input.transition.pending_prefill_requests ? 1U : 0U;
                if (!std::ranges::equal(std::span(source.filters).subspan(source_prefix), std::span(invocation.input.filters).subspan(target_prefix))) {
                    if (!transform_gap)
                        transform_gap = TemplateGap{ TemplateGapKind::Rule,
                                                     "batch_update_filter_transform_not_implemented",
                                                     invocation.step,
                                                     invocation.input.phase,
                                                     rank };
                    continue;
                }
            }
            if (!source.admission) {
                if (!transform_gap)
                    transform_gap =
                        TemplateGap{ TemplateGapKind::Observation, "admission_entry_state_not_observed", invocation.step, invocation.input.phase, rank };
                continue;
            }
            // Decode carries running requests, but admits no new prefill members.
            const auto admitted = invocation.input.phase == BatchPhase::Prefill ? invocation.input.requests.size() : 0;
            const bool parameterized_admission = source.admission_cpu_request_count
                                                 && (*source.admission_cpu_request_count == admitted || (admitted > 0 && !source.admission_regions.empty()))
                                                 && invocation.input.admission_work.parameterized_cpu_path(invocation.input.admission, admitted);
            if (*source.admission != invocation.input.admission && !parameterized_admission) {
                if (!transform_gap)
                    transform_gap =
                        TemplateGap{ TemplateGapKind::Rule, "admission_prefix_work_transform_not_implemented", invocation.step, invocation.input.phase, rank };
                continue;
            }
            if (!source.admission_work || (*source.admission_work != invocation.input.admission_work && !parameterized_admission)) {
                if (!transform_gap)
                    transform_gap = TemplateGap{ source.admission_work ? TemplateGapKind::Rule : TemplateGapKind::Observation,
                                                 source.admission_work ? "admission_work_transform_not_implemented" : "admission_work_not_qualified",
                                                 invocation.step,
                                                 invocation.input.phase,
                                                 rank };
                continue;
            }
        }

        if (invocation.step == StepKind::Schedule && !source.transition_region
            && (source.filters != invocation.input.filters || source.merge != invocation.input.merge)) {
            // Observed membership is present; changing the call sequence or its
            // branch requires a rewrite, not another observation of token shapes.
            std::string reason = "batch_merge_branch_rewrite_not_implemented";
            if (source.filters != invocation.input.filters)
                reason = source.filters.size() == invocation.input.filters.size() ? "batch_filter_membership_transform_not_implemented"
                                                                                  : "batch_filter_sequence_rewrite_not_implemented";
            if (!transform_gap) transform_gap = TemplateGap{ TemplateGapKind::Rule, std::move(reason), invocation.step, invocation.input.phase, rank };
            continue;
        }
        auto candidate = instantiate_source(source, invocation, request_capacity);
        if (!candidate) {
            // A matching observation can still lie outside a parameter or
            // rewrite domain. Other sources must get the same qualification.
            if (!transform_gap) transform_gap = TemplateGap{ TemplateGapKind::Rule, candidate.error(), invocation.step, invocation.input.phase, rank };
            continue;
        }

        selected.emplace(std::move(*candidate));
        break;
    }

    if (!batch_observed)
        return std::unexpected(SelectionFailure{
            MatchStage::Source,
            { TemplateGapKind::Observation, "native_batch_step_not_observed", invocation.step, invocation.input.phase, rank }
        });
    if (!request_count_match)
        return std::unexpected(SelectionFailure{
            MatchStage::RequestCount,
            { TemplateGapKind::Rule, "request_count_transform_not_implemented", invocation.step, invocation.input.phase, rank }
        });
    if (!publication_match)
        return std::unexpected(SelectionFailure{
            MatchStage::Publication,
            { publication_rewrite_missing ? TemplateGapKind::Rule : TemplateGapKind::Observation,
                           publication_rewrite_missing ? "publication_member_branch_rewrite_not_implemented" : "native_publication_branch_not_observed",
                           invocation.step,
                           invocation.input.phase,
                           rank }
        });
    if (!token_shape_match)
        return std::unexpected(SelectionFailure{
            MatchStage::Shape,
            { TemplateGapKind::Rule,
                     length_domain_rejected ? "request_length_outside_parameter_domain" : "internal_shape_transform_not_implemented",
                     invocation.step,
                     invocation.input.phase,
                     rank }
        });
    if (!selected) return std::unexpected(SelectionFailure{ MatchStage::Structure, *transform_gap });
    return std::move(*selected);
}

std::expected<TemplateInstance, std::string> TemplateBank::instantiate_source(const TemplateDefinition & source, const InvokeStep & invocation,
                                                                              std::uint32_t request_capacity) const {
    TemplateInstance instance(invocation.work_id, source);
    const auto rewrites = plan_materializations(source, invocation.input);
    if (!rewrites) return std::unexpected(rewrites.error());
    const auto applied = instance.topology.apply_operations(source, *rewrites);
    if (!applied) return std::unexpected(applied.error());

    for (const auto index : instance.topology.nodes()) {
        const auto & node = source.graph[index];
        const auto & node_observation = source.observation(node);
        const auto transfer = source.transfer_layouts.find(node_observation.source_span);
        if (node_observation.transfer && transfer != source.transfer_layouts.end()) {
            auto target = transfer->second.evaluate(*node_observation.transfer, invocation.input);
            if (!target) return std::unexpected(target.error());
            instance.transfer_overrides.emplace(node_observation.source_span, *target);
        }

        if (!node_observation.kernel || instance.kernel_overrides.contains(node_observation.source_span)) continue;
        auto target = transform_kernel(node_observation, source, invocation, request_capacity);
        if (!target) return std::unexpected(target.error());
        if (!*target) continue;

        // Model layouts already carry identity relations in diagnostics; only
        // materialized descriptor changes need an extra override record.
        const auto & kernel = **target;
        if (source.model_kernel_layouts.contains(node_observation.source_span) && kernel.input_shapes == node_observation.kernel->input_shapes
            && kernel.output_shapes == node_observation.kernel->output_shapes && !kernel.logits_token_indices && !kernel.positions && !kernel.page_table
            && !kernel.sequence_length_max && !kernel.sequence_lengths)
            continue;
        instance.kernel_overrides.emplace(node_observation.source_span, std::move(**target));
    }

    if (source.publication_layout) {
        auto output = source.publication_layout->evaluate(invocation.input);
        if (!output) return std::unexpected(output.error());
        const bool retained = std::ranges::any_of(instance.topology.nodes(), [&](std::size_t index) {
            return source.observation(source.graph[index]).source_span == output->source_span;
        });
        if (!retained) return std::unexpected("publication_output_region_replacement_not_qualified");
        instance.publication_work = *output;
    }

    const auto inserted = parameterize_insertions(instance.topology, invocation, request_capacity);
    if (!inserted) return std::unexpected(inserted.error());
    return instance;
}

std::expected<std::optional<SourceKernel>, std::string> TemplateBank::transform_kernel(const TemplateNodeObservation & observation,
                                                                                       const TemplateDefinition & source, const InvokeStep & invocation,
                                                                                       std::uint32_t request_capacity, std::optional<std::size_t> request_slot,
                                                                                       std::optional<BatchFilterBinding> filter_binding) const {
    // Production binds component semantics before generic shape matching.
    // Inserted nodes use their donor's binding with this instance's target state.
    const auto kv = source.kv_kernel_layouts.find(observation.source_span);
    if (kv != source.kv_kernel_layouts.end()) {
        if (!kv->second) return std::unexpected(kv->second.error());
        auto target = kv->second->evaluate(*observation.kernel, invocation.input, request_slot);
        if (!target) return std::unexpected(target.error());
        return std::move(*target);
    }

    const auto batch = source.batch_kernel_layouts.find(observation.source_span);
    if (batch != source.batch_kernel_layouts.end()) {
        if (!batch->second) return std::unexpected(batch->second.error());
        auto target = filter_binding ? batch->second->with_filter_binding(*filter_binding).evaluate(*observation.kernel, invocation.input, request_slot)
                                     : batch->second->evaluate(*observation.kernel, invocation.input, request_slot);
        if (!target) return std::unexpected(target.error());
        return std::move(*target);
    }

    const auto attention = source.attention_kernel_layouts.find(observation.source_span);
    if (attention != source.attention_kernel_layouts.end()) return attention->second.rule->evaluate(*observation.kernel, invocation.input);
    const auto model = source.model_kernel_layouts.find(observation.source_span);
    if (model != source.model_kernel_layouts.end()) return source.graph.model_rule(model->second.node).evaluate(*observation.kernel, invocation.input);

    const auto pool = source.request_pool_layouts.find(observation.source_span);
    if (pool != source.request_pool_layouts.end()) {
        if (!pool->second) return std::unexpected(pool->second.error());
        auto target = pool->second->evaluate(*observation.kernel, invocation.input, request_capacity);
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
                    auto target = layout.evaluate(*inserted.observation().transfer, invocation.input);
                    if (!target) return std::unexpected(target.error());
                    topology.bind_inserted_transfer(index, *target);
                }
                else return std::unexpected("inserted_transfer_parameter_relation_missing");
            }
            else if (inserted.node.kind == SourceSpanKind::Dma) return std::unexpected("inserted_transfer_observation_missing");
            if (inserted.node.kind == SourceSpanKind::Compute) return std::unexpected("inserted_kernel_observation_missing");
            continue;
        }
        auto target =
            transform_kernel(inserted.observation(), *inserted.definition, invocation, request_capacity, inserted.request_slot, inserted.filter_binding);
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
    if (source_.initialization_gap)
        return std::unexpected(TemplateGap{ TemplateGapKind::Observation, *source_.initialization_gap, StepKind::Schedule, BatchPhase::Idle, 0 });
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


    std::size_t observed_groups = 0;
    std::size_t source_instances = 0;
    std::size_t shared_regions = 0;
    std::size_t model_rule_observations = 0;
    std::size_t attention_rule_observations = 0;
    std::size_t collective_rule_observations = 0;
    auto observations = Json::array();
    std::set<TemplateResource> resources;
    const ServingContract contract;
    for (const auto & [key, instances] : source_.definitions) {
        if (!instances.empty()) ++observed_groups;
        source_instances += instances.size();

        // Each row is a source call. A step/phase/rank bucket is not a
        // structural family, and repeated observations are not new variants.
        for (const auto & source : instances) {
            for (const auto & lane : source.lanes)
                if (lane.source.domain == SourceDomain::CpuThread || lane.source.domain == SourceDomain::DeviceStream) resources.insert(lane.resource());
            for (const auto & wait : source.stream_waits) resources.insert(wait.resources.begin(), wait.resources.end());
            for (const auto & connection : source.connection_observations)
                if (connection.resource) resources.insert(*connection.resource);

            shared_regions += source.graph.shared_regions();
            model_rule_observations += source.model_kernel_layouts.size();
            attention_rule_observations += source.attention_kernel_layouts.size();
            collective_rule_observations += source.collectives.size();
            auto requests = Json::array();
            for (std::size_t slot = 0; slot < source.source_requests.size(); ++slot) {
                const auto & request = source.source_requests[slot];
                const auto publication = source.publication_finishes[slot];
                requests.push_back({
                    {     "input_tokens",                             request.input_tokens },
                    {   "context_tokens",                           request.context_tokens },
                    { "generated_tokens",                         request.generated_tokens },
                    {         "finishes", publication ? Json(*publication) : Json(nullptr) }
                });
            }
            auto filters = Json::array();
            for (const auto & filter : source.filters) filters.push_back(filter.describe());
            auto initialization_regions = Json::array();
            for (const auto index : source.request_initialization_regions) initialization_regions.push_back(source.regions[index].source_call);

            auto length_domain = Json(nullptr);
            if (source.length_domain) {
                length_domain = {
                    { "minimum_context_tokens", source.length_domain->minimum_context_tokens },
                    { "maximum_context_tokens", source.length_domain->maximum_context_tokens },
                    {   "minimum_input_tokens",   source.length_domain->minimum_input_tokens },
                    {   "maximum_input_tokens",   source.length_domain->maximum_input_tokens }
                };
            }
            Json admission_regions = Json::array();
            for (const auto index : source.admission_regions) admission_regions.push_back(source.regions[index].source_call);

            Json publication_regions = Json::array();
            for (const auto index : source.publication_regions) publication_regions.push_back(source.regions[index].source_call);

            // Expose the producer's existing branch qualification, not a new
            // coverage heuristic. Node layouts and target ports still qualify
            // independently during instantiation.
            Json kv_region = nullptr;
            if (source.kv_region) {
                const auto & branch = *source.kv_region;
                kv_region = {
                    {          "kind", branch.kind == KvRegionKind::Allocation ? "allocation" : "release_group" },
                    {   "source_call",                                source.regions[branch.region].source_call },
                    { "changes_pages",                                                     branch.changes_pages }
                };
                if (branch.kind == KvRegionKind::ReleaseGroup) {
                    kv_region["minimum_members"] = branch.minimum_members;
                    kv_region["maximum_members"] = branch.maximum_members;
                }
            }

            // Report existing facts and binding qualification separately. An
            // observed branch is not a promise that all targets can reuse it.
            observations.push_back({
                { "transition", source.transition ? source.transition->describe() : Json(nullptr) },
                { "admission_entry", source.admission ? source.admission->describe() : Json(nullptr) },
                { "admission_work",
                 source.admission && source.admission_work
                      ? source.admission_work->describe(*source.admission, source.phase == BatchPhase::Prefill ? source.source_requests.size() : 0)
                      : Json(nullptr) },
                { "admission_cpu_request_count", source.admission_cpu_request_count ? Json(*source.admission_cpu_request_count) : Json(nullptr) },
                { "admission_regions", admission_regions },
                { "transition_cpu_parameters", source.transition_cpu_parameters },
                { "transition_merge_parameters", source.transition_merge_parameters },
                { "running_update_parameters", source.running_update_region.has_value() },
                { "running_update_cpu_only",
                 source.running_update_region ? Json(source.is_cpu_region(source.regions[*source.running_update_region])) : Json(nullptr) },
                { "publication_regions", publication_regions },
                { "kv_region", kv_region },
                { "periodic_output_interval_tokens", source.output_policy ? Json(source.output_policy->interval_tokens) : Json(nullptr) },
                { "length_domain", length_domain },
                { "step", contract.block(source.step).name },
                { "phase", ServingContract{}.phase_name(std::get<1>(key)) },
                { "rank", source.rank },
                { "source_batch", source.source_batch ? Json(*source.source_batch) : Json(nullptr) },
                { "source_call", source.source_call },
                { "requests", requests },
                { "request_initialization_regions", initialization_regions },
                { "batch_filters", filters },
                { "batch_merge", source.merge ? source.merge->describe() : Json(nullptr) }
            });
        }
    }

    auto resource_bindings = Json::array();
    for (const auto & resource : resources) resource_bindings.push_back(resource.describe());

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

    return {
        {                  "initialization",                                        initialization_status                                            },
        {                       "workspace",
         { { "source_binding", source_.workspace_binding_gap ? "needs_data" : "available" },
         { "binding_gap", source_.workspace_binding_gap ? Json(*source_.workspace_binding_gap) : Json(nullptr) },
         { "pools", workspace_pools },
         { "operation_uses", workspace_uses },
         { "demand_rules", workspace_demand_rules },
         { "demand_rule_definitions", source_.workspace_rules.size() },
         { "demand_gaps", workspace_demand_gaps },
         { "required_device_synchronizations", workspace_synchronizations },
         { "target_state", "partial" } }                                                                                                             },
        {        "sequence_capacity_tokens",               source_.sequence_capacity_tokens ? Json(*source_.sequence_capacity_tokens) : Json(nullptr) },
        {            "static_qualification",                                                                                     source_qualification },
        {             "request_pool_domain",
         { { "maximum", source_.request_pool_domain.maximum },
         { "shrink_gap", source_.request_pool_domain.shrink_gap ? Json(*source_.request_pool_domain.shrink_gap) : Json(nullptr) } }                  },
        {                "admission_policy",                                                                                                admission },
        {                        "manifest",                                                                           source_.manifest_path.string() },
        {                      "resolution",                                                                                       source_.resolution },
        {                "page_size_tokens",                                                                                 source_.page_size_tokens },
        { "observed_step_phase_rank_groups",                                                                                          observed_groups },
        {                "source_instances",                                                                                         source_instances },
        {        "region_structure_sharing",
         { { "definitions", source_.region_structures.size() },
         { "regions", shared_regions },
         { "scope", "nodes_dependencies_model_and_collective_rules_resources_and_interfaces" } }                                                     },
        {       "attention_parameter_rules",   { { "definitions", source_.attention_rules.size() }, { "observations", attention_rule_observations } } },
        {           "model_parameter_rules",           { { "definitions", source_.model_rules.size() }, { "observations", model_rule_observations } } },
        {      "collective_parameter_rules", { { "definitions", source_.collective_rules.size() }, { "observations", collective_rule_observations } } },
        {               "resource_bindings",                                                                                        resource_bindings },
        {                    "observations",                                                                                             observations },
        {                     "source_gaps",                                                                                             source_.gaps },
        {                      "transforms",                                                                                       source_.transforms },
        {                          "limits",                                                                                           source_.limits }
    };
}

} // namespace markov
