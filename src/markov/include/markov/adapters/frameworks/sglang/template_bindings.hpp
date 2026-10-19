#pragma once

#include "markov/contracts/source_observations.hpp"
#include "markov/production/block_binding.hpp"
#include "markov/production/source_call.hpp"
#include "markov/templates/communication_work.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>

namespace markov {

struct TemplateSource;
struct TemplateDefinition;
class RequestPoolLayout;
class SGLangCopyLayout;
class ModelParameterRule;
enum class ModelKernelRole;
class AttentionParameterRule;
class SourceGraph;
struct SourceSubgraphs;

// Produce an owning template source and interpret native scopes during that
// construction. Consumers receive layouts, dependencies and operation identity.
class SGLangTemplateBindings {
public:
    // Qualified contiguous INT64 scalar-list branch of ACLNN ConcatD.
    static constexpr std::uint64_t last_location_concat_limit = 32;
    static TemplateSource produce(const std::filesystem::path & manifest_path, const nlohmann::json & manifest, const SourceGraph & graph,
                                  const SourceSubgraphs & subgraphs, std::span<const SourceCall> calls, const nlohmann::json & facts);

    SGLangTemplateBindings(const SourceGraph & source, TemplateDefinition & definition, std::set<ModelParameterRule> & rules,
                           std::set<CollectiveParameterRule> & collectives, std::set<AttentionParameterRule> & attention)
        : source_(source),
          definition_(definition),
          collective_rules_(collectives),
          attention_rules_(attention),
          model_rules_(rules) {}
    void bind_membership(std::span<const SourceBatchFilter> filters);
    void bind_publication(std::span<const SourcePublication> publications, const std::optional<SourceOutputPolicy> & policy);
    void bind_kv(std::uint32_t page_size_tokens);
    void bind_length_domain(std::uint32_t page_size_tokens, std::optional<std::uint32_t> sequence_capacity_tokens);
    void bind_request_pool(const RequestPoolLayout & pool);
    void bind(std::span<const SourceCall> calls, const SGLangCopyLayout & copies, std::uint32_t page_size_tokens);

private:
    void bind_regions(std::span<const SourceCall> calls);
    void bind_admission_work();
    void bind_transition_merge();
    void bind_publication_progress();
    void bind_publication_output();
    void bind_layouts(std::span<const SourceCall> calls, std::uint32_t page_size_tokens);
    void bind_last_locations(std::uint32_t page_size_tokens);
    void bind_request_initialization();
    void bind_decode_locations();
    void bind_release_transfer(std::uint64_t sequence_tokens);
    void bind_prefix_publication();
    void bind_rotary_positions();
    std::map<std::size_t, CollectiveCountRule> bind_collectives() const;
    void bind_logits_reshape();
    std::optional<std::size_t> logits_gather_reshape() const;

    std::map<std::size_t, SourceNativeRegion> native_regions_; // Construction facts; never serialized into the template.
    std::map<SourceSpanId, ModelKernelRole> model_roles_;      // Native recognition is independent of the shared parameter rule's value.
    const SourceGraph & source_;                               // Borrowed native records; consumers receive only compiled rules.
    TemplateDefinition & definition_;                          // Borrowed only while producing one definition.
    std::set<CollectiveParameterRule> & collective_rules_;
    std::set<AttentionParameterRule> & attention_rules_;
    std::set<ModelParameterRule> & model_rules_; // Owned by the produced source.
};

} // namespace markov
